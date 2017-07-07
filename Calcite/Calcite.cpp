/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * File:   Calcite.cpp
 * Author: michael
 *
 * Created on November 23, 2015, 9:33 AM
 */

#include "Calcite.h"
#include "Shared/measure.h"
#include "../Shared/mapdpath.h"

#include <glog/logging.h>
#include <thread>
#include <utility>

using namespace std;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;

void Calcite::runJNI(const int port, const std::string& data_dir, const size_t calcite_max_mem) {
  LOG(INFO) << "Creating Calcite Server local as JNI instance, jar expected in " << mapd_root_abs_path() << "/bin";
  const int kNumOptions = 2;
  std::string jar_file{"-Djava.class.path=" + mapd_root_abs_path() +
                       "/bin/calcite-1.0-SNAPSHOT-jar-with-dependencies.jar"};
  std::string max_mem_setting{"-Xmx" + std::to_string(calcite_max_mem) + "m"};
  JavaVMOption options[kNumOptions] = {{const_cast<char*>(max_mem_setting.c_str()), NULL},
                                       {const_cast<char*>(jar_file.c_str()), NULL}};

  JavaVMInitArgs vm_args;
  vm_args.version = JNI_VERSION_1_6;
  vm_args.options = options;
  vm_args.nOptions = sizeof(options) / sizeof(JavaVMOption);
  assert(vm_args.nOptions == kNumOptions);

  JNIEnv* env;
  int res = JNI_CreateJavaVM(&jvm_, reinterpret_cast<void**>(&env), &vm_args);
  CHECK_EQ(res, JNI_OK);

  calciteDirect_ = env->FindClass("com/mapd/parser/server/CalciteDirect");

  CHECK(calciteDirect_);

  // now call the constructor
  constructor_ = env->GetMethodID(calciteDirect_, "<init>", "(ILjava/lang/String;Ljava/lang/String;)V");
  CHECK(constructor_);

  // create the new calciteDirect via call to constructor
  const auto extension_functions_ast_file = mapd_root_abs_path() + "/QueryEngine/ExtensionFunctions.ast";
  calciteDirectObject_ = env->NewGlobalRef(env->NewObject(calciteDirect_,
                                                          constructor_,
                                                          port,
                                                          env->NewStringUTF(data_dir.c_str()),
                                                          env->NewStringUTF(extension_functions_ast_file.c_str())));
  CHECK(calciteDirectObject_);

  // get all the methods we will need for calciteDirect;
  processMID_ = env->GetMethodID(calciteDirect_,
                                 "process",
                                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZZ)Lcom/"
                                 "mapd/parser/server/CalciteReturn;");
  CHECK(processMID_);

  // get all the methods we will need for calciteDirect;
  updateMetadataMID_ = env->GetMethodID(calciteDirect_,
                                        "updateMetadata",
                                        "(Ljava/lang/String;Ljava/lang/String;)"
                                        "Lcom/mapd/parser/server/CalciteReturn;");
  CHECK(updateMetadataMID_);

  getExtensionFunctionWhitelistMID_ =
      env->GetMethodID(calciteDirect_, "getExtensionFunctionWhitelist", "()Ljava/lang/String;");
  CHECK(getExtensionFunctionWhitelistMID_);

  // get all the methods we will need to process the calcite results
  jclass calcite_return_class = env->FindClass("com/mapd/parser/server/CalciteReturn");
  CHECK(calcite_return_class);

  hasFailedMID_ = env->GetMethodID(calcite_return_class, "hasFailed", "()Z");
  CHECK(hasFailedMID_);
  getElapsedTimeMID_ = env->GetMethodID(calcite_return_class, "getElapsedTime", "()J");
  CHECK(getElapsedTimeMID_);
  getTextMID_ = env->GetMethodID(calcite_return_class, "getText", "()Ljava/lang/String;");
  CHECK(getTextMID_);
}

void start_calcite_server_as_daemon(const int mapd_port,
                                    const int port,
                                    const std::string& data_dir,
                                    const size_t calcite_max_mem) {
  // todo MAT all platforms seem to respect /usr/bin/java - this could be a gotcha on some weird thing
  std::string xmxP = "-Xmx" + std::to_string(calcite_max_mem) + "m";
  std::string jarP = "-jar";
  std::string jarD = mapd_root_abs_path() + "/bin/calcite-1.0-SNAPSHOT-jar-with-dependencies.jar";
  std::string extensionsP = "-e";
  std::string extensionsD = mapd_root_abs_path() + "/QueryEngine/";
  std::string dataP = "-d";
  std::string dataD = data_dir;
  std::string localPortP = "-p" + std::to_string(port);
  std::string localPortD = std::to_string(port);
  std::string mapdPortP = "-m";
  std::string mapdPortD = "-1";

  int pid = fork();
  if (pid == 0) {
    int i = execl("/usr/bin/java",
                  xmxP.c_str(),
                  jarP.c_str(),
                  jarD.c_str(),
                  extensionsP.c_str(),
                  extensionsD.c_str(),
                  dataP.c_str(),
                  dataD.c_str(),
                  localPortP.c_str(),
                  localPortD.c_str(),
                  mapdPortP.c_str(),
                  mapdPortD.c_str(),
                  (char*)0);
    LOG(INFO) << " Calcite server running after exe, return " << i;
  }
}

std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> get_client(int port) {
  boost::shared_ptr<TTransport> socket(new TSocket("localhost", port));
  boost::shared_ptr<TTransport> transport(new TBufferedTransport(socket));
  try {
    transport->open();

  } catch (TException& tx) {
    throw tx;
  }
  boost::shared_ptr<TProtocol> protocol(new TBinaryProtocol(transport));
  boost::shared_ptr<CalciteServerClient> client;
  client.reset(new CalciteServerClient(protocol));
  std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> ret;
  return std::make_pair(client, transport);
}

void Calcite::runServer(const int mapd_port,
                        const int port,
                        const std::string& data_dir,
                        const size_t calcite_max_mem) {
  LOG(INFO) << "Running calcite server as a daemon";

  // ping server to see if for any reason there is an orphaned one
  int ping_time = ping();
  if (ping_time > -1) {
    // we have an orphaned server shut it down
    LOG(ERROR) << "Appears to be orphaned Calcite serve already running, shutting it down";
    LOG(ERROR) << "Please check that you are not trying to run two servers on same port";
    LOG(ERROR) << "Attempting to shutdown orphaned Calcite server";
    try {
      std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> clientP =
          get_client(remote_calcite_port_);
      clientP.first->shutdown();
      clientP.second->close();
      LOG(ERROR) << "orphaned Calcite server shutdown";

    } catch (TException& tx) {
      LOG(ERROR) << "Failed to shutdown orphaned Calcite server, reason: " << tx.what();
    }
  }

  // start the calcite server as a seperate process
  start_calcite_server_as_daemon(mapd_port, port, data_dir, calcite_max_mem);

  // check for new server for 5 seconds max
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  for (int i = 2; i < 50; i++) {
    int ping_time = ping();
    if (ping_time > -1) {
      LOG(INFO) << "Calcite server start took " << i * 100 << " ms " << endl;
      LOG(INFO) << "ping took " << ping_time << " ms " << endl;
      server_available_ = true;
      jni_ = false;
      return;
    } else {
      // wait 100 ms
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  server_available_ = false;
  LOG(FATAL) << "No calcite remote server running on port " << port;
}

// ping existing server
// return -1 if no ping response
int Calcite::ping() {
  try {
    auto ms = measure<>::execution([&]() {
      std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> clientP =
          get_client(remote_calcite_port_);
      clientP.first->ping();
      clientP.second->close();
    });
    return ms;

  } catch (TException& tx) {
    return -1;
  }
}

Calcite::Calcite(const int mapd_port, const int port, const std::string& data_dir, const size_t calcite_max_mem)
    : server_available_(false), jni_(true), jvm_(NULL) {
  LOG(INFO) << "Creating Calcite Handler,  Calcite Port is " << port << " base data dir is " << data_dir;
  if (port == -1) {
    runJNI(mapd_port, data_dir, calcite_max_mem);
    jni_ = true;
    server_available_ = false;
    return;
  }
  if (port == 0) {
    // dummy process for initdb
    remote_calcite_port_ = port;
    server_available_ = false;
    jni_ = false;
  } else {
    remote_calcite_port_ = port;
    runServer(mapd_port, port, data_dir, calcite_max_mem);
    server_available_ = true;
    jni_ = false;
  }
}

JNIEnv* Calcite::checkJNIConnection() {
  JNIEnv* env;
  int res = jvm_->GetEnv((void**)&env, JNI_VERSION_1_6);
  if (res != JNI_OK) {
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;  // choose your JNI version
    args.name = NULL;                // you might want to give the java thread a name
    args.group = NULL;               // you might want to assign the java thread to a ThreadGroup
    int res = jvm_->AttachCurrentThread((void**)&env, &args);
    if (res != JNI_OK) {
      throw std::runtime_error("Failed to create a JNI interface pointer");
    }
  }
  CHECK(calciteDirectObject_);
  CHECK(processMID_);
  CHECK(updateMetadataMID_);

  return env;
}

void Calcite::updateMetadata(string catalog, string table) {
  if (jni_) {
    JNIEnv* env = checkJNIConnection();
    jobject process_result;
    auto ms = measure<>::execution([&]() {
      process_result = env->CallObjectMethod(calciteDirectObject_,
                                             updateMetadataMID_,
                                             env->NewStringUTF(catalog.c_str()),
                                             env->NewStringUTF(table.c_str()));

    });
    if (env->ExceptionCheck()) {
      LOG(ERROR) << "Exception occurred ";
      env->ExceptionDescribe();
      LOG(ERROR) << "Exception occurred " << env->ExceptionOccurred();
      throw std::runtime_error("Calcite::updateMetadata failed");
    }
    LOG(INFO) << "Time to updateMetadata " << ms << " (ms)" << endl;
  } else {
    if (server_available_) {
      auto ms = measure<>::execution([&]() {
        std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> clientP =
            get_client(remote_calcite_port_);
        clientP.first->updateMetadata(catalog, table);
        clientP.second->close();
      });
      LOG(INFO) << "Time to updateMetadata " << ms << " (ms)" << endl;
    } else {
      LOG(INFO) << "Not routing to Calcite, server is not up and JNI not available" << endl;
    }
  }
}

string Calcite::process(string user,
                        string session,
                        string catalog,
                        string sql_string,
                        const bool legacy_syntax,
                        const bool is_explain) {
  LOG(INFO) << "User " << user << " catalog " << catalog << " sql '" << sql_string << "'";
  if (jni_) {
    JNIEnv* env = checkJNIConnection();
    jboolean legacy = legacy_syntax;
    jobject process_result;
    auto ms = measure<>::execution([&]() {
      process_result = env->CallObjectMethod(calciteDirectObject_,
                                             processMID_,
                                             env->NewStringUTF(user.c_str()),
                                             env->NewStringUTF(session.c_str()),
                                             env->NewStringUTF(catalog.c_str()),
                                             env->NewStringUTF(sql_string.c_str()),
                                             legacy,
                                             is_explain);

    });
    if (env->ExceptionCheck()) {
      LOG(ERROR) << "Exception occurred ";
      env->ExceptionDescribe();
      LOG(ERROR) << "Exception occurred " << env->ExceptionOccurred();
      throw std::runtime_error("Calcite::process failed");
    }
    CHECK(process_result);
    long java_time = env->CallLongMethod(process_result, getElapsedTimeMID_);

    LOG(INFO) << "Time marshalling in JNI " << (ms > java_time ? ms - java_time : 0) << " (ms), Time in Java Calcite  "
              << java_time << " (ms)" << endl;
    return handle_java_return(env, process_result);
  } else {
    if (server_available_) {
      TPlanResult ret;
      try {
        auto ms = measure<>::execution([&]() {

          std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> clientP =
              get_client(remote_calcite_port_);
          clientP.first->process(ret, user, session, catalog, sql_string, legacy_syntax, is_explain);
          clientP.second->close();
        });

        // LOG(INFO) << ret.plan_result << endl;
        LOG(INFO) << "Time in Thrift " << (ms > ret.execution_time_ms ? ms - ret.execution_time_ms : 0)
                  << " (ms), Time in Java Calcite server " << ret.execution_time_ms << " (ms)" << endl;
        return ret.plan_result;
      } catch (InvalidParseRequest& e) {
        throw std::invalid_argument(e.whyUp);
      }
    } else {
      LOG(INFO) << "Not routing to Calcite, server is not up and JNI not available" << endl;
      return "";
    }
  }
}

string Calcite::handle_java_return(JNIEnv* env, jobject process_result) {
  CHECK(process_result);
  CHECK(getTextMID_);
  jstring s = (jstring)env->CallObjectMethod(process_result, getTextMID_);
  CHECK(s);
  // convert the Java String to use it in C
  jboolean iscopy;
  const char* text = env->GetStringUTFChars(s, &iscopy);

  bool failed = env->CallBooleanMethod(process_result, hasFailedMID_);
  if (failed) {
    LOG(ERROR) << "Calcite process failed " << text;
    string retText(text);
    env->ReleaseStringUTFChars(s, text);
    env->DeleteLocalRef(process_result);
    jvm_->DetachCurrentThread();
    throw std::invalid_argument(retText);
  }
  string retText(text);
  env->ReleaseStringUTFChars(s, text);
  env->DeleteLocalRef(process_result);
  jvm_->DetachCurrentThread();
  return retText;
}

string Calcite::getExtensionFunctionWhitelist() {
  if (jni_) {
    JNIEnv* env = checkJNIConnection();
    const auto whitelist_result =
        static_cast<jstring>(env->CallObjectMethod(calciteDirectObject_, getExtensionFunctionWhitelistMID_));
    if (env->ExceptionCheck()) {
      LOG(ERROR) << "Exception occurred ";
      env->ExceptionDescribe();
      LOG(ERROR) << "Exception occurred " << env->ExceptionOccurred();
      throw std::runtime_error("Calcite::getExtensionFunctionWhitelist failed");
    }
    CHECK(whitelist_result);
    jboolean iscopy;
    const char* whitelist_cstr = env->GetStringUTFChars(whitelist_result, &iscopy);
    string whitelist(whitelist_cstr);
    env->ReleaseStringUTFChars(whitelist_result, whitelist_cstr);
    env->DeleteLocalRef(whitelist_result);
    jvm_->DetachCurrentThread();
    return whitelist;
  } else {
    if (server_available_) {
      TPlanResult ret;
      string whitelist;

      std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> clientP =
          get_client(remote_calcite_port_);
      clientP.first->getExtensionFunctionWhitelist(whitelist);
      clientP.second->close();
      LOG(INFO) << whitelist << endl;
      return whitelist;
    } else {
      LOG(INFO) << "Not routing to Calcite, server is not up and JNI not available" << endl;
      return "";
    }
  }
  CHECK(false);
  return "";
}

Calcite::~Calcite() {
  LOG(INFO) << "Destroy Calcite Class" << std::endl;
  if (jvm_) {
    jvm_->DestroyJavaVM();
  } else {
    if (server_available_) {
      // running server
      std::pair<boost::shared_ptr<CalciteServerClient>, boost::shared_ptr<TTransport>> clientP =
          get_client(remote_calcite_port_);
      clientP.first->shutdown();
      clientP.second->close();
    }
  }
  LOG(INFO) << "End of Calcite Destructor ";
}
