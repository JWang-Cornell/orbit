// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "CaptureData.h"
#include "FunctionUtils.h"
#include "OrbitClientModel/CaptureSerializer.h"
#include "OrbitProcess.h"
#include "TracepointCustom.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "capture_data.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using orbit_client_protos::CallstackEvent;
using orbit_client_protos::CallstackInfo;
using orbit_client_protos::CaptureInfo;
using orbit_client_protos::FunctionInfo;
using orbit_client_protos::FunctionStats;
using orbit_client_protos::LinuxAddressInfo;
using ::testing::ElementsAreArray;

TEST(CaptureSerializer, GetCaptureFileName) {
  CaptureData capture_data;

  time_t timestamp = std::chrono::system_clock::to_time_t(capture_data.capture_start_time());
  std::string expected_file_name = absl::StrCat("_", OrbitUtils::FormatTime(timestamp), ".orbit");
  EXPECT_EQ(expected_file_name, capture_serializer::GetCaptureFileName(capture_data));
}

TEST(CaptureSerializer, IncludeOrbitExtensionInFile) {
  std::string file_name_with_extension = "process_000.orbit";
  std::string expected_file_name = file_name_with_extension;
  capture_serializer::IncludeOrbitExtensionInFile(file_name_with_extension);
  EXPECT_EQ(expected_file_name, file_name_with_extension);

  std::string file_name_without_extension = "process_000";
  capture_serializer::IncludeOrbitExtensionInFile(file_name_without_extension);
  EXPECT_EQ(expected_file_name, file_name_without_extension);
}

TEST(CaptureSerializer, GenerateCaptureInfoEmpty) {
  CaptureData capture_data;
  absl::flat_hash_map<uint64_t, std::string> key_to_string_map;

  CaptureInfo capture_info =
      capture_serializer::internal::GenerateCaptureInfo(capture_data, key_to_string_map);
  EXPECT_EQ(0, capture_info.selected_functions_size());
  EXPECT_EQ(-1, capture_info.process_id());
  EXPECT_EQ("", capture_info.process_name());
  EXPECT_EQ(0, capture_info.address_infos_size());
  EXPECT_EQ(0, capture_info.callstacks_size());
  EXPECT_EQ(0, capture_info.callstack_events_size());
  EXPECT_EQ(0, capture_info.key_to_string_size());
  EXPECT_EQ(0, capture_info.function_stats_size());
}

TEST(CaptureSerializer, GenerateCaptureInfo) {
  int32_t process_id = 42;
  std::string process_name = "p";
  auto process = std::make_shared<Process>();
  process->SetName(process_name);
  process->SetID(process_id);

  absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo> selected_functions;
  FunctionInfo selected_function;
  selected_function.set_name("foo");
  selected_function.set_address(123);
  selected_function.set_module_base_address(15);
  selected_functions[FunctionUtils::GetAbsoluteAddress(selected_function)] = selected_function;

  TracepointInfoSet selected_tracepoints;
  CaptureData capture_data{process_id, process_name, process, selected_functions,
                           selected_tracepoints};

  LinuxAddressInfo address_info;
  address_info.set_absolute_address(987);
  address_info.set_offset_in_function(0);
  capture_data.InsertAddressInfo(address_info);

  std::vector<uint64_t> addresses;
  addresses.push_back(1);
  addresses.push_back(2);
  addresses.push_back(3);
  CallStack callstack(std::move(addresses));
  capture_data.AddUniqueCallStack(callstack);

  CallstackEvent callstack_event;
  callstack_event.set_time(1);
  callstack_event.set_thread_id(123);
  callstack_event.set_callstack_hash(callstack.GetHash());
  capture_data.AddCallstackEvent(callstack_event);

  capture_data.UpdateFunctionStats(selected_function, 100);
  capture_data.UpdateFunctionStats(selected_function, 110);
  capture_data.UpdateFunctionStats(selected_function, 120);

  absl::flat_hash_map<uint64_t, std::string> key_to_string_map;
  key_to_string_map[0] = "a";
  key_to_string_map[1] = "b";
  key_to_string_map[2] = "c";

  CaptureInfo capture_info =
      capture_serializer::internal::GenerateCaptureInfo(capture_data, key_to_string_map);

  ASSERT_EQ(1, capture_info.selected_functions_size());
  const FunctionInfo& actual_selected_function = capture_info.selected_functions(0);
  EXPECT_EQ(selected_function.address(), actual_selected_function.address());
  EXPECT_EQ(selected_function.name(), actual_selected_function.name());

  EXPECT_EQ(process_id, capture_info.process_id());
  EXPECT_EQ(process_name, capture_info.process_name());

  ASSERT_EQ(1, capture_info.address_infos_size());
  const LinuxAddressInfo& actual_address_info = capture_info.address_infos(0);
  EXPECT_EQ(address_info.absolute_address(), actual_address_info.absolute_address());
  EXPECT_EQ(address_info.offset_in_function(), actual_address_info.offset_in_function());

  ASSERT_EQ(1, capture_info.callstacks_size());
  const CallstackInfo& actual_callstack = capture_info.callstacks(0);
  std::vector<uint64_t> actual_callstack_data{actual_callstack.data().begin(),
                                              actual_callstack.data().end()};
  EXPECT_THAT(actual_callstack_data, ElementsAreArray(callstack.GetFrames()));

  ASSERT_EQ(1, capture_info.callstack_events_size());
  const CallstackEvent& actual_callstack_event = capture_info.callstack_events(0);
  EXPECT_EQ(callstack_event.thread_id(), actual_callstack_event.thread_id());
  EXPECT_EQ(callstack_event.time(), actual_callstack_event.time());
  EXPECT_EQ(callstack_event.callstack_hash(), actual_callstack_event.callstack_hash());

  ASSERT_EQ(1, capture_info.function_stats_size());
  ASSERT_TRUE(
      capture_info.function_stats().contains(FunctionUtils::GetAbsoluteAddress(selected_function)));
  const FunctionStats& actual_function_stats =
      capture_info.function_stats().at(FunctionUtils::GetAbsoluteAddress(selected_function));
  const FunctionStats& expected_function_stats =
      capture_data.GetFunctionStatsOrDefault(selected_function);
  EXPECT_EQ(expected_function_stats.count(), actual_function_stats.count());
  EXPECT_EQ(expected_function_stats.total_time_ns(), actual_function_stats.total_time_ns());
  EXPECT_EQ(expected_function_stats.average_time_ns(), actual_function_stats.average_time_ns());
  EXPECT_EQ(expected_function_stats.min_ns(), actual_function_stats.min_ns());
  EXPECT_EQ(expected_function_stats.max_ns(), actual_function_stats.max_ns());

  ASSERT_EQ(key_to_string_map.size(), capture_info.key_to_string_size());
  for (const auto& expected_key_to_string : key_to_string_map) {
    ASSERT_TRUE(capture_info.key_to_string().contains(expected_key_to_string.first));
    EXPECT_EQ(expected_key_to_string.second,
              capture_info.key_to_string().at(expected_key_to_string.first));
  }
}