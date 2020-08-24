// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/Server.h"
#include "server/init/InstanceLockCheck.h"

#include <fcntl.h>
#include <unistd.h>
#include <boost/filesystem.hpp>
#include <cstring>
#include <unordered_map>

#include "config/ServerConfig.h"

#include "log/LogMgr.h"
// #include "scheduler/SchedInst.h"
#include "server/grpc_impl/GrpcServer.h"
#include "server/init/CpuChecker.h"
// #include "server/init/GpuChecker.h"
#include "server/init/StorageChecker.h"
#include "src/version.h"
#include <yaml-cpp/yaml.h>
#include "utils/Log.h"
#include "utils/SignalHandler.h"
#include "utils/TimeRecorder.h"

namespace milvus {
namespace server {

Server&
Server::GetInstance() {
    static Server server;
    return server;
}

void
Server::Init(int64_t daemonized, const std::string& pid_filename, const std::string& config_filename) {
    daemonized_ = daemonized;
    pid_filename_ = pid_filename;
    config_filename_ = config_filename;
}

void
Server::Daemonize() {
    if (daemonized_ == 0) {
        return;
    }

    std::cout << "Milvus server run in daemonize mode";

    pid_t pid = 0;

    // Fork off the parent process
    pid = fork();

    // An error occurred
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    // Success: terminate parent
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // On success: The child process becomes session leader
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    // Ignore signal sent from child to parent process
    signal(SIGCHLD, SIG_IGN);

    // Fork off for the second time
    pid = fork();

    // An error occurred
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    // Terminate the parent
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Set new file permissions
    umask(0);

    // Change the working directory to root
    int ret = chdir("/");
    if (ret != 0) {
        return;
    }

    // Close all open fd
    for (int64_t fd = sysconf(_SC_OPEN_MAX); fd > 0; fd--) {
        close(fd);
    }

    std::cout << "Redirect stdin/stdout/stderr to /dev/null";

    // Redirect stdin/stdout/stderr to /dev/null
    stdin = fopen("/dev/null", "r");
    stdout = fopen("/dev/null", "w+");
    stderr = fopen("/dev/null", "w+");
    // Try to write PID of daemon to lockfile
    if (!pid_filename_.empty()) {
        pid_fd_ = open(pid_filename_.c_str(), O_RDWR | O_CREAT, 0640);
        if (pid_fd_ < 0) {
            std::cerr << "Can't open filename: " + pid_filename_ + ", Error: " + strerror(errno);
            exit(EXIT_FAILURE);
        }
        if (lockf(pid_fd_, F_TLOCK, 0) < 0) {
            std::cerr << "Can't lock filename: " + pid_filename_ + ", Error: " + strerror(errno);
            exit(EXIT_FAILURE);
        }

        std::string pid_file_context = std::to_string(getpid());
        ssize_t res = write(pid_fd_, pid_file_context.c_str(), pid_file_context.size());
        if (res != 0) {
            return;
        }
    }
}

Status
Server::Start() {
    if (daemonized_ != 0) {
        Daemonize();
    }

    try {
        auto meta_uri = config.general.meta_uri();
        if (meta_uri.length() > 6 && strcasecmp("sqlite", meta_uri.substr(0, 6).c_str()) == 0) {
            std::cout << "WARNING: You are using SQLite as the meta data management, "
                         "which can't be used in production. Please change it to MySQL!"
                      << std::endl;
        }

        /* Init opentracing tracer from config */
        std::string tracing_config_path = config.tracing.json_config_path();
        tracing_config_path.empty() ? tracing::TracerUtil::InitGlobal()
                                    : tracing::TracerUtil::InitGlobal(tracing_config_path);

        auto time_zone = config.general.timezone();

        if (time_zone.length() == 3) {
            time_zone = "CUT";
        } else {
            int time_bias = std::stoi(time_zone.substr(3, std::string::npos));
            if (time_bias == 0) {
                time_zone = "CUT";
            } else if (time_bias > 0) {
                time_zone = "CUT" + std::to_string(-time_bias);
            } else {
                time_zone = "CUT+" + std::to_string(-time_bias);
            }
        }

        if (setenv("TZ", time_zone.c_str(), 1) != 0) {
            return Status(SERVER_UNEXPECTED_ERROR, "Fail to setenv");
        }
        tzset();

        /* log path is defined in Config file, so InitLog must be called after LoadConfig */
        STATUS_CHECK(LogMgr::InitLog(config.logs.trace.enable(), config.logs.level(), config.logs.path(),
                                     config.logs.max_log_file_size(), config.logs.log_rotate_num()));

        bool cluster_enable = config.cluster.enable();
        auto cluster_role = config.cluster.role();

        Status s;
        if ((not cluster_enable) || cluster_role == ClusterRole::RW) {
            try {
                // True if a new directory was created, otherwise false.
                boost::filesystem::create_directories(config.storage.path());
            } catch (std::exception& ex) {
                return Status(SERVER_UNEXPECTED_ERROR, "Cannot create db directory, " + std::string(ex.what()));
            } catch (...) {
                return Status(SERVER_UNEXPECTED_ERROR, "Cannot create db directory");
            }

            s = InstanceLockCheck::Check(config.storage.path());
            if (!s.ok()) {
                if (not cluster_enable) {
                    std::cerr << "single instance lock db path failed." << s.message() << std::endl;
                } else {
                    std::cerr << cluster_role << " instance lock db path failed." << s.message() << std::endl;
                }
                return s;
            }

            if (config.wal.enable()) {
                std::string wal_path = config.wal.path();

                try {
                    // True if a new directory was created, otherwise false.
                    boost::filesystem::create_directories(wal_path);
                } catch (...) {
                    return Status(SERVER_UNEXPECTED_ERROR, "Cannot create wal directory");
                }
                s = InstanceLockCheck::Check(wal_path);
                if (!s.ok()) {
                    if (not cluster_enable) {
                        std::cerr << "single instance lock wal path failed." << s.message() << std::endl;
                    } else {
                        std::cerr << cluster_role << " instance lock wal path failed." << s.message() << std::endl;
                    }
                    return s;
                }
            }
        }

        // print version information
        LOG_SERVER_INFO_ << "Milvus " << BUILD_TYPE << " version: v" << MILVUS_VERSION << ", built at " << BUILD_TIME;
#ifdef MILVUS_GPU_VERSION
        LOG_SERVER_INFO_ << "GPU edition";
#else
        LOG_SERVER_INFO_ << "CPU edition";
#endif
        STATUS_CHECK(StorageChecker::CheckStoragePermission());
        STATUS_CHECK(CpuChecker::CheckCpuInstructionSet());

        /* record config and hardware information into log */
        LogConfigInFile(config_filename_);
        LogCpuInfo();
        LOG_SERVER_INFO_ << "\n\n"
                         << std::string(15, '*') << "Config in memory" << std::string(15, '*') << "\n\n"
                         << ConfigMgr::GetInstance().Dump();

        server::Metrics::GetInstance().Init();
        server::SystemInfo::GetInstance().Init();

        return StartService();
    } catch (std::exception& ex) {
        std::string str = "Milvus server encounter exception: " + std::string(ex.what());
        return Status(SERVER_UNEXPECTED_ERROR, str);
    }
}

void
Server::Stop() {
    std::cerr << "Milvus server is going to shutdown ..." << std::endl;

    /* Unlock and close lockfile */
    if (pid_fd_ != -1) {
        int ret = lockf(pid_fd_, F_ULOCK, 0);
        if (ret != 0) {
            std::cerr << "ERROR: Can't lock file: " << strerror(errno) << std::endl;
            exit(0);
        }
        ret = close(pid_fd_);
        if (ret != 0) {
            std::cerr << "ERROR: Can't close file: " << strerror(errno) << std::endl;
            exit(0);
        }
    }

    /* delete lockfile */
    if (!pid_filename_.empty()) {
        int ret = unlink(pid_filename_.c_str());
        if (ret != 0) {
            std::cerr << "ERROR: Can't unlink file: " << strerror(errno) << std::endl;
            exit(0);
        }
    }

    StopService();

    std::cerr << "Milvus server exit..." << std::endl;
}

Status
Server::StartService() {
    Status stat;
    stat = engine::KnowhereResource::Initialize();
    if (!stat.ok()) {
        LOG_SERVER_ERROR_ << "KnowhereResource initialize fail: " << stat.message();
        goto FAIL;
    }

    grpc::GrpcServer::GetInstance().Start();

    // stat = storage::S3ClientWrapper::GetInstance().StartService();
    // if (!stat.ok()) {
    //     LOG_SERVER_ERROR_ << "S3Client start service fail: " << stat.message();
    //     goto FAIL;
    // }

    return Status::OK();
FAIL:
    std::cerr << "Milvus initializes fail: " << stat.message() << std::endl;
    return stat;
}

void
Server::StopService() {
    // storage::S3ClientWrapper::GetInstance().StopService();
    grpc::GrpcServer::GetInstance().Stop();
}

void
Server::LogConfigInFile(const std::string& path) {
    // TODO(yhz): Check if file exists
    auto node = YAML::LoadFile(path);
    YAML::Emitter out;
    out << node;
    LOG_SERVER_INFO_ << "\n\n"
                     << std::string(15, '*') << "Config in file" << std::string(15, '*') << "\n\n"
                     << out.c_str();
}

void
Server::LogCpuInfo() {
    /*CPU information*/
    std::fstream fcpu("/proc/cpuinfo", std::ios::in);
    if (!fcpu.is_open()) {
        LOG_SERVER_WARNING_ << "Cannot obtain CPU information. Open file /proc/cpuinfo fail: " << strerror(errno)
                            << "(errno: " << errno << ")";
        return;
    }
    std::stringstream cpu_info_ss;
    cpu_info_ss << fcpu.rdbuf();
    fcpu.close();
    std::string cpu_info = cpu_info_ss.str();

    auto processor_pos = cpu_info.rfind("processor");
    if (std::string::npos == processor_pos) {
        LOG_SERVER_WARNING_ << "Cannot obtain CPU information. No sub string \'processor\'";
        return;
    }

    auto sub_str = cpu_info.substr(processor_pos);
    LOG_SERVER_INFO_ << "\n\n" << std::string(15, '*') << "CPU" << std::string(15, '*') << "\n\n" << sub_str;
}

}  // namespace server
}  // namespace milvus
