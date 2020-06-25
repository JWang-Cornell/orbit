// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <csignal>
#include <iostream>

#include "OrbitBase/Logging.h"
#include "OrbitService.h"
#include "Path.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "capture.pb.h"

ABSL_FLAG(uint16_t, asio_port, 44766, "Asio TCP server port");

ABSL_FLAG(uint64_t, grpc_port, 44765, "Grpc server port");

ABSL_FLAG(bool, devmode, false, "Enable developer mode");

// TODO: Remove this flag once we enable specifying the sampling frequency or
//  period in the client.
ABSL_FLAG(uint16_t, sampling_rate, 1000,
          "Frequency of callstack sampling in samples per second");

// TODO: Remove this flag once we have a ui option to specify.
ABSL_FLAG(bool, frame_pointer_unwinding, false,
          "Use frame pointers for unwinding");

namespace {
std::atomic<bool> exit_requested;

void sigint_handler(int signum) {
  if (signum == SIGINT) {
    exit_requested = true;
  }
}

void install_sigint_handler() {
  struct sigaction act;
  act.sa_handler = sigint_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  act.sa_restorer = nullptr;
  sigaction(SIGINT, &act, nullptr);
}
}  // namespace

int main(int argc, char** argv) {
  const std::string log_file_path = Path::GetServiceLogFilePath();
  InitLogFile(log_file_path);

  absl::SetProgramUsageMessage("Orbit CPU Profiler Service");
  absl::ParseCommandLine(argc, argv);

  install_sigint_handler();

  uint16_t grpc_port = absl::GetFlag(FLAGS_grpc_port);
  uint16_t asio_port = absl::GetFlag(FLAGS_asio_port);

  CaptureOptions capture_options;
  capture_options.set_sampling_rate(absl::GetFlag(FLAGS_sampling_rate));
  capture_options.set_trace_gpu_driver(true);
  capture_options.set_trace_context_switches(true);

  if (absl::GetFlag(FLAGS_frame_pointer_unwinding)) {
    capture_options.set_unwinding_method(CaptureOptions::kFramePointers);
  } else {
    capture_options.set_unwinding_method(CaptureOptions::kDwarf);
  }

  exit_requested = false;
  OrbitService service{grpc_port, asio_port, capture_options};
  service.Run(&exit_requested);
}
