// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "TimeGraph.h"

#include <OrbitBase/Logging.h>
#include <OrbitBase/Profiling.h>
#include <OrbitBase/Tracing.h>

#include <algorithm>
#include <utility>

#include "../Orbit.h"
#include "App.h"
#include "Batcher.h"
#include "EventTrack.h"
#include "FunctionUtils.h"
#include "Geometry.h"
#include "GlCanvas.h"
#include "GpuTrack.h"
#include "GraphTrack.h"
#include "Params.h"
#include "PickingManager.h"
#include "SamplingProfiler.h"
#include "SchedulerTrack.h"
#include "StringManager.h"
#include "TextBox.h"
#include "TextRenderer.h"
#include "ThreadTrack.h"
#include "Utils.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_format.h"

using orbit_client_protos::CallstackEvent;
using orbit_client_protos::FunctionInfo;
using orbit_client_protos::TimerInfo;

TimeGraph* GCurrentTimeGraph = nullptr;

TimeGraph::TimeGraph() : batcher_(BatcherId::kTimeGraph) {
  last_thread_reorder_.Start();
  scheduler_track_ = GetOrCreateSchedulerTrack();

  // The process track is a special ThreadTrack of id "kAllThreadsFakeTid".
  process_track_ = GetOrCreateThreadTrack(SamplingProfiler::kAllThreadsFakeTid);

  async_timer_info_listener_ =
      std::make_unique<ManualInstrumentationManager::AsyncTimerInfoListener>(
          [this](const std::string& name, const TimerInfo& timer_info) {
            ProcessAsyncTimer(name, timer_info);
          });
  manual_instrumentation_manager_ = GOrbitApp->GetManualInstrumentationManager();
  manual_instrumentation_manager_->AddAsyncTimerListener(async_timer_info_listener_.get());
}

TimeGraph::~TimeGraph() {
  manual_instrumentation_manager_->RemoveAsyncTimerListener(async_timer_info_listener_.get());
}

Color TimeGraph::GetColor(uint32_t id) const {
  constexpr unsigned char kAlpha = 255;
  static std::vector<Color> colors{
      Color(231, 68, 53, kAlpha),    // red
      Color(43, 145, 175, kAlpha),   // blue
      Color(185, 117, 181, kAlpha),  // purple
      Color(87, 166, 74, kAlpha),    // green
      Color(215, 171, 105, kAlpha),  // beige
      Color(248, 101, 22, kAlpha)    // orange
  };
  return colors[id % colors.size()];
}

Color TimeGraph::GetColor(uint64_t id) const { return GetColor(static_cast<uint32_t>(id)); }

Color TimeGraph::GetColor(const std::string& str) const { return GetColor(StringHash(str)); }

Color TimeGraph::GetThreadColor(int32_t tid) const { return GetColor(static_cast<uint32_t>(tid)); }

void TimeGraph::SetStringManager(std::shared_ptr<StringManager> str_manager) {
  string_manager_ = std::move(str_manager);
}

void TimeGraph::SetCanvas(GlCanvas* a_Canvas) {
  canvas_ = a_Canvas;
  text_renderer_->SetCanvas(a_Canvas);
  text_renderer_static_.SetCanvas(a_Canvas);
  batcher_.SetPickingManager(&a_Canvas->GetPickingManager());
}

void TimeGraph::SetFontSize(int a_FontSize) {
  text_renderer_->SetFontSize(a_FontSize);
  text_renderer_static_.SetFontSize(a_FontSize);
}

void TimeGraph::Clear() {
  ScopeLock lock(mutex_);

  batcher_.StartNewFrame();
  capture_min_timestamp_ = std::numeric_limits<uint64_t>::max();
  capture_max_timestamp_ = 0;
  thread_count_map_.clear();

  tracks_.clear();
  scheduler_track_ = nullptr;
  thread_tracks_.clear();
  gpu_tracks_.clear();
  graph_tracks_.clear();
  async_tracks_.clear();

  cores_seen_.clear();
  scheduler_track_ = GetOrCreateSchedulerTrack();

  // The process track is a special ThreadTrack of id "kAllThreadsFakeTid".
  process_track_ = GetOrCreateThreadTrack(SamplingProfiler::kAllThreadsFakeTid);

  SetIteratorOverlayData({}, {});

  NeedsUpdate();
}

double GNumHistorySeconds = 2.f;

bool TimeGraph::UpdateCaptureMinMaxTimestamps() {
  capture_min_timestamp_ = std::numeric_limits<uint64_t>::max();

  mutex_.lock();
  for (auto& track : tracks_) {
    if (track->GetNumTimers() > 0) {
      uint64_t min = track->GetMinTime();
      if (min > 0 && min < capture_min_timestamp_) {
        capture_min_timestamp_ = min;
      }
    }
  }
  mutex_.unlock();

  if (GOrbitApp->GetCaptureData().GetCallstackData()->GetCallstackEventsCount() > 0) {
    capture_min_timestamp_ = std::min(capture_min_timestamp_,
                                      GOrbitApp->GetCaptureData().GetCallstackData()->min_time());
    capture_max_timestamp_ = std::max(capture_max_timestamp_,
                                      GOrbitApp->GetCaptureData().GetCallstackData()->max_time());
  }

  return capture_min_timestamp_ != std::numeric_limits<uint64_t>::max();
}

void TimeGraph::ZoomAll() {
  if (UpdateCaptureMinMaxTimestamps()) {
    max_time_us_ = TicksToMicroseconds(capture_min_timestamp_, capture_max_timestamp_);
    min_time_us_ = max_time_us_ - (GNumHistorySeconds * 1000 * 1000);
    if (min_time_us_ < 0) min_time_us_ = 0;

    NeedsUpdate();
  }
}

void TimeGraph::Zoom(uint64_t min, uint64_t max) {
  double start = TicksToMicroseconds(capture_min_timestamp_, min);
  double end = TicksToMicroseconds(capture_min_timestamp_, max);

  double mid = start + ((end - start) / 2.0);
  double extent = 1.1 * (end - start) / 2.0;

  SetMinMax(mid - extent, mid + extent);
}

void TimeGraph::Zoom(const TextBox* a_TextBox) {
  const TimerInfo& timer_info = a_TextBox->GetTimerInfo();
  Zoom(timer_info.start(), timer_info.end());
}

double TimeGraph::GetCaptureTimeSpanUs() {
  if (UpdateCaptureMinMaxTimestamps()) {
    return TicksToMicroseconds(capture_min_timestamp_, capture_max_timestamp_);
  }

  return 0.0;
}

double TimeGraph::GetCurrentTimeSpanUs() { return max_time_us_ - min_time_us_; }

void TimeGraph::ZoomTime(float a_ZoomValue, double a_MouseRatio) {
  zoom_value_ = a_ZoomValue;
  mouse_ratio_ = a_MouseRatio;

  static double incrementRatio = 0.1;
  double scale = (a_ZoomValue > 0) ? (1 + incrementRatio) : (1 / (1 + incrementRatio));

  double CurrentTimeWindowUs = max_time_us_ - min_time_us_;
  ref_time_us_ = min_time_us_ + a_MouseRatio * CurrentTimeWindowUs;

  double timeLeft = std::max(ref_time_us_ - min_time_us_, 0.0);
  double timeRight = std::max(max_time_us_ - ref_time_us_, 0.0);

  double minTimeUs = ref_time_us_ - scale * timeLeft;
  double maxTimeUs = ref_time_us_ + scale * timeRight;

  if (maxTimeUs - minTimeUs < 0.001 /*1 ns*/) {
    return;
  }

  SetMinMax(minTimeUs, maxTimeUs);
}

void TimeGraph::VerticalZoom(float zoom_value, float mouse_relative_position) {
  constexpr float increment_ratio = 0.1f;

  const float ratio = (zoom_value > 0) ? (1 + increment_ratio) : (1 / (1 + increment_ratio));

  const float world_height = canvas_->GetWorldHeight();
  const float y_mouse_position =
      canvas_->GetWorldTopLeftY() - mouse_relative_position * world_height;
  const float top_distance = canvas_->GetWorldTopLeftY() - y_mouse_position;

  const float new_y_mouse_position = y_mouse_position / ratio;

  float new_world_top_left_y = new_y_mouse_position + top_distance;

  // If we zoomed-out, we would like to see most part of the screen with events,
  // so we set a minimum and maximum for the y-top coordinate.
  new_world_top_left_y = std::max(new_world_top_left_y, world_height - GetThreadTotalHeight());
  // TODO: TopMargin has to be 1.5f * layout_.GetSliderWidth()?
  new_world_top_left_y = std::min(new_world_top_left_y, 1.5f * layout_.GetSliderWidth());

  canvas_->SetWorldTopLeftY(new_world_top_left_y);

  // Finally, we have to scale every item in the layout.
  const float old_scale = layout_.GetScale();
  layout_.SetScale(old_scale / ratio);
}

void TimeGraph::SetMinMax(double a_MinTimeUs, double a_MaxTimeUs) {
  double desiredTimeWindow = a_MaxTimeUs - a_MinTimeUs;
  min_time_us_ = std::max(a_MinTimeUs, 0.0);
  max_time_us_ = std::min(min_time_us_ + desiredTimeWindow, GetCaptureTimeSpanUs());

  NeedsUpdate();
}

void TimeGraph::PanTime(int a_InitialX, int a_CurrentX, int a_Width, double a_InitialTime) {
  time_window_us_ = max_time_us_ - min_time_us_;
  double initialLocalTime = static_cast<double>(a_InitialX) / a_Width * time_window_us_;
  double dt = static_cast<double>(a_CurrentX - a_InitialX) / a_Width * time_window_us_;
  double currentTime = a_InitialTime - dt;
  min_time_us_ =
      clamp(currentTime - initialLocalTime, 0.0, GetCaptureTimeSpanUs() - time_window_us_);
  max_time_us_ = min_time_us_ + time_window_us_;

  NeedsUpdate();
}

void TimeGraph::HorizontallyMoveIntoView(VisibilityType vis_type, uint64_t min, uint64_t max,
                                         double distance) {
  if (IsVisible(vis_type, min, max)) {
    return;
  }

  double start = TicksToMicroseconds(capture_min_timestamp_, min);
  double end = TicksToMicroseconds(capture_min_timestamp_, max);

  double CurrentTimeWindowUs = max_time_us_ - min_time_us_;

  if (vis_type == VisibilityType::kFullyVisible && CurrentTimeWindowUs < (end - start)) {
    Zoom(min, max);
    return;
  }

  double mid = start + ((end - start) / 2.0);

  // Mirror the final center position if we have to move left
  if (start < min_time_us_) {
    distance = 1 - distance;
  }

  SetMinMax(mid - CurrentTimeWindowUs * (1 - distance), mid + CurrentTimeWindowUs * distance);

  NeedsUpdate();
}

void TimeGraph::HorizontallyMoveIntoView(VisibilityType vis_type, const TextBox* text_box,
                                         double distance) {
  HorizontallyMoveIntoView(vis_type, text_box->GetTimerInfo().start(),
                           text_box->GetTimerInfo().end(), distance);
}

void TimeGraph::VerticallyMoveIntoView(const TextBox* text_box) {
  CHECK(text_box != nullptr);
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  auto thread_track = GetOrCreateThreadTrack(timer_info.thread_id());
  auto text_box_y_position = thread_track->GetYFromDepth(timer_info.depth());

  float world_top_left_y = canvas_->GetWorldTopLeftY();
  float min_world_top_left_y =
      text_box_y_position + layout_.GetSpaceBetweenTracks() + layout_.GetTopMargin();
  float max_world_top_left_y = text_box_y_position + canvas_->GetWorldHeight() -
                               GetTextBoxHeight() - layout_.GetBottomMargin();
  CHECK(min_world_top_left_y <= max_world_top_left_y);
  world_top_left_y = std::min(world_top_left_y, max_world_top_left_y);
  world_top_left_y = std::max(world_top_left_y, min_world_top_left_y);
  canvas_->SetWorldTopLeftY(world_top_left_y);
  NeedsUpdate();
}

void TimeGraph::OnDrag(float a_Ratio) {
  double timeSpan = GetCaptureTimeSpanUs();
  double timeWindow = max_time_us_ - min_time_us_;
  min_time_us_ = a_Ratio * (timeSpan - timeWindow);
  max_time_us_ = min_time_us_ + timeWindow;
}

double TimeGraph::GetTime(double a_Ratio) const {
  double CurrentWidth = max_time_us_ - min_time_us_;
  double Delta = a_Ratio * CurrentWidth;
  return min_time_us_ + Delta;
}

double TimeGraph::GetTimeIntervalMicro(double a_Ratio) {
  double CurrentWidth = max_time_us_ - min_time_us_;
  return a_Ratio * CurrentWidth;
}

void TimeGraph::ProcessTimer(const TimerInfo& timer_info, const FunctionInfo* function) {
  if (timer_info.end() > capture_max_timestamp_) {
    capture_max_timestamp_ = timer_info.end();
  }

  if (function != nullptr && function->orbit_type() != FunctionInfo::kNone) {
    ProcessOrbitFunctionTimer(function->orbit_type(), timer_info);
  }

  if (timer_info.type() == TimerInfo::kGpuActivity) {
    uint64_t timeline_hash = timer_info.timeline_hash();
    std::shared_ptr<GpuTrack> track = GetOrCreateGpuTrack(timeline_hash);
    track->OnTimer(timer_info);
  } else {
    std::shared_ptr<ThreadTrack> track = GetOrCreateThreadTrack(timer_info.thread_id());
    if (timer_info.type() == TimerInfo::kIntrospection) {
      const Color kGreenIntrospection(87, 166, 74, 255);
      track->SetColor(kGreenIntrospection);
    }

    if (timer_info.type() != TimerInfo::kCoreActivity) {
      track->OnTimer(timer_info);
      ++thread_count_map_[timer_info.thread_id()];
    } else {
      scheduler_track_->OnTimer(timer_info);
      cores_seen_.insert(timer_info.processor());
    }
  }

  NeedsUpdate();
}

void TimeGraph::ProcessOrbitFunctionTimer(FunctionInfo::OrbitType type,
                                          const TimerInfo& timer_info) {
  switch (type) {
    case FunctionInfo::kOrbitTrackValue:
      ProcessValueTrackingTimer(timer_info);
      break;
    case FunctionInfo::kOrbitTimerStartAsync:
      [[fallthrough]];
    case FunctionInfo::kOrbitTimerStopAsync:
      manual_instrumentation_manager_->ProcessAsyncTimer(timer_info);
      break;
    default:
      break;
  }
}

void TimeGraph::ProcessValueTrackingTimer(const TimerInfo& timer_info) {
  orbit_api::Event event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);

  if (event.type == orbit_api::kString) {
    manual_instrumentation_manager_->ProcessStringEvent(event);
    return;
  }

  auto track = GetOrCreateGraphTrack(event.name);
  uint64_t time = timer_info.start();

  switch (event.type) {
    case orbit_api::kTrackInt: {
      track->AddValue(orbit_api::Decode<int32_t>(event.value), time);
    } break;
    case orbit_api::kTrackInt64: {
      track->AddValue(orbit_api::Decode<int64_t>(event.value), time);
    } break;
    case orbit_api::kTrackUint: {
      track->AddValue(orbit_api::Decode<uint32_t>(event.value), time);
    } break;
    case orbit_api::kTrackUint64: {
      track->AddValue(event.value, time);
    } break;
    case orbit_api::kTrackFloat: {
      track->AddValue(orbit_api::Decode<float>(event.value), time);
    } break;
    case orbit_api::kTrackDouble: {
      track->AddValue(orbit_api::Decode<double>(event.value), time);
    } break;
    default:
      ERROR("Unsupported value tracking type [%u]", event.type);
      break;
  }
}

void TimeGraph::ProcessAsyncTimer(const std::string& track_name, const TimerInfo& timer_info) {
  auto track = GetOrCreateAsyncTrack(track_name);
  track->OnTimer(timer_info);
}

uint32_t TimeGraph::GetNumTimers() const {
  uint32_t numTimers = 0;
  ScopeLock lock(mutex_);
  for (const auto& track : tracks_) {
    numTimers += track->GetNumTimers();
  }
  return numTimers;
}

uint32_t TimeGraph::GetNumCores() const {
  ScopeLock lock(mutex_);
  return cores_seen_.size();
}

std::vector<std::shared_ptr<TimerChain>> TimeGraph::GetAllTimerChains() const {
  std::vector<std::shared_ptr<TimerChain>> chains;
  for (const auto& track : tracks_) {
    Append(chains, track->GetAllChains());
  }
  return chains;
}

std::vector<std::shared_ptr<TimerChain>> TimeGraph::GetAllThreadTrackTimerChains() const {
  std::vector<std::shared_ptr<TimerChain>> chains;
  for (const auto& [_, track] : thread_tracks_) {
    Append(chains, track->GetAllChains());
  }
  return chains;
}

void TimeGraph::UpdateMaxTimeStamp(uint64_t a_Time) {
  if (a_Time > capture_max_timestamp_) {
    capture_max_timestamp_ = a_Time;
  }
};

float TimeGraph::GetThreadTotalHeight() { return std::abs(min_y_); }

float TimeGraph::GetWorldFromTick(uint64_t a_Time) const {
  if (time_window_us_ > 0) {
    double start = TicksToMicroseconds(capture_min_timestamp_, a_Time) - min_time_us_;
    double normalizedStart = start / time_window_us_;
    float pos = float(world_start_x_ + normalizedStart * world_width_);
    return pos;
  }

  return 0;
}

float TimeGraph::GetWorldFromUs(double a_Micros) const {
  return GetWorldFromTick(GetTickFromUs(a_Micros));
}

double TimeGraph::GetUsFromTick(uint64_t time) const {
  return TicksToMicroseconds(capture_min_timestamp_, time) - min_time_us_;
}

uint64_t TimeGraph::GetTickFromWorld(float world_x) const {
  double ratio =
      world_width_ != 0 ? static_cast<double>((world_x - world_start_x_) / world_width_) : 0;
  uint64_t time_span_ns = static_cast<uint64_t>(1000 * GetTime(ratio));
  return capture_min_timestamp_ + time_span_ns;
}

uint64_t TimeGraph::GetTickFromUs(double micros) const {
  uint64_t nanos = static_cast<uint64_t>(1000 * micros);
  return capture_min_timestamp_ + nanos;
}

void TimeGraph::GetWorldMinMax(float& a_Min, float& a_Max) const {
  a_Min = GetWorldFromTick(capture_min_timestamp_);
  a_Max = GetWorldFromTick(capture_max_timestamp_);
}

void TimeGraph::Select(const TextBox* text_box) {
  GOrbitApp->SelectTextBox(text_box);
  HorizontallyMoveIntoView(VisibilityType::kPartlyVisible, text_box);
  VerticallyMoveIntoView(text_box);
}

const TextBox* TimeGraph::FindPreviousFunctionCall(uint64_t function_address, uint64_t current_time,
                                                   std::optional<int32_t> thread_ID) const {
  TextBox* previous_box = nullptr;
  uint64_t previous_box_time = std::numeric_limits<uint64_t>::lowest();
  std::vector<std::shared_ptr<TimerChain>> chains = GetAllThreadTrackTimerChains();
  for (auto& chain : chains) {
    if (!chain) continue;
    for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
      TimerBlock& block = *it;
      if (!block.Intersects(previous_box_time, current_time)) continue;
      for (uint64_t i = 0; i < block.size(); i++) {
        TextBox& box = block[i];
        auto box_time = box.GetTimerInfo().end();
        if ((box.GetTimerInfo().function_address() == function_address) &&
            (!thread_ID || thread_ID.value() == box.GetTimerInfo().thread_id()) &&
            (box_time < current_time) && (previous_box_time < box_time)) {
          previous_box = &box;
          previous_box_time = box_time;
        }
      }
    }
  }
  return previous_box;
}

const TextBox* TimeGraph::FindNextFunctionCall(uint64_t function_address, uint64_t current_time,
                                               std::optional<int32_t> thread_ID) const {
  TextBox* next_box = nullptr;
  uint64_t next_box_time = std::numeric_limits<uint64_t>::max();
  std::vector<std::shared_ptr<TimerChain>> chains = GetAllThreadTrackTimerChains();
  for (auto& chain : chains) {
    if (!chain) continue;
    for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
      TimerBlock& block = *it;
      if (!block.Intersects(current_time, next_box_time)) continue;
      for (uint64_t i = 0; i < block.size(); i++) {
        TextBox& box = block[i];
        auto box_time = box.GetTimerInfo().end();
        if ((box.GetTimerInfo().function_address() == function_address) &&
            (!thread_ID || thread_ID.value() == box.GetTimerInfo().thread_id()) &&
            (box_time > current_time) && (next_box_time > box_time)) {
          next_box = &box;
          next_box_time = box_time;
        }
      }
    }
  }
  return next_box;
}

void TimeGraph::NeedsUpdate() {
  needs_update_primitives_ = true;
  // If the primitives need to be updated, we also have to redraw.
  needs_redraw_ = true;
}

void TimeGraph::UpdatePrimitives(PickingMode picking_mode) {
  CHECK(string_manager_);

  batcher_.StartNewFrame();
  text_renderer_static_.Clear();

  UpdateMaxTimeStamp(GOrbitApp->GetCaptureData().GetCallstackData()->max_time());

  time_window_us_ = max_time_us_ - min_time_us_;
  world_start_x_ = canvas_->GetWorldTopLeftX();
  world_width_ = canvas_->GetWorldWidth();
  uint64_t min_tick = GetTickFromUs(min_time_us_);
  uint64_t max_tick = GetTickFromUs(max_time_us_);

  SortTracks();

  float current_y = -layout_.GetSchedulerTrackOffset();

  for (auto& track : sorted_tracks_) {
    track->SetY(current_y);
    track->UpdatePrimitives(min_tick, max_tick, picking_mode);
    current_y -= (track->GetHeight() + layout_.GetSpaceBetweenTracks());
  }

  min_y_ = current_y;
  needs_update_primitives_ = false;
}

std::vector<CallstackEvent> TimeGraph::SelectEvents(float world_start, float world_end,
                                                    int32_t thread_id) {
  if (world_start > world_end) {
    std::swap(world_end, world_start);
  }

  uint64_t t0 = GetTickFromWorld(world_start);
  uint64_t t1 = GetTickFromWorld(world_end);

  std::vector<CallstackEvent> selected_callstack_events =
      (thread_id == SamplingProfiler::kAllThreadsFakeTid)
          ? GOrbitApp->GetCaptureData().GetCallstackData()->GetCallstackEventsInTimeRange(t0, t1)
          : GOrbitApp->GetCaptureData().GetCallstackData()->GetCallstackEventsOfTidInTimeRange(
                thread_id, t0, t1);

  selected_callstack_events_per_thread_.clear();
  for (CallstackEvent& event : selected_callstack_events) {
    selected_callstack_events_per_thread_[event.thread_id()].emplace_back(event);
    selected_callstack_events_per_thread_[SamplingProfiler::kAllThreadsFakeTid].emplace_back(event);
  }

  GOrbitApp->SelectCallstackEvents(selected_callstack_events, thread_id);

  NeedsUpdate();

  return selected_callstack_events;
}

const std::vector<CallstackEvent>& TimeGraph::GetSelectedCallstackEvents(int32_t tid) {
  return selected_callstack_events_per_thread_[tid];
}

void TimeGraph::Draw(GlCanvas* canvas, PickingMode picking_mode) {
  current_mouse_time_ns_ = GetTickFromWorld(canvas_->GetMouseX());

  const bool picking = picking_mode != PickingMode::kNone;
  if ((!picking && needs_update_primitives_) || picking) {
    UpdatePrimitives(picking_mode);
  }

  DrawTracks(canvas, picking_mode);
  DrawOverlay(canvas, picking_mode);
  batcher_.Draw(picking);

  needs_redraw_ = false;
}

namespace {

[[nodiscard]] std::string GetLabelBetweenIterators(const FunctionInfo& function_a,
                                                   const FunctionInfo& function_b) {
  std::string function_from = FunctionUtils::GetDisplayName(function_a);
  std::string function_to = FunctionUtils::GetDisplayName(function_b);
  return absl::StrFormat("%s to %s", function_from, function_to);
}

std::string GetTimeString(const TextBox* box_a, const TextBox* box_b) {
  absl::Duration duration =
      TicksToDuration(box_a->GetTimerInfo().start(), box_b->GetTimerInfo().start());

  return GetPrettyTime(duration);
}

[[nodiscard]] Color GetIteratorBoxColor(uint64_t index) {
  constexpr uint64_t kNumColors = 2;
  const Color kLightBlueGray = Color(177, 203, 250, 60);
  const Color kMidBlueGray = Color(81, 102, 157, 60);
  Color colors[kNumColors] = {kLightBlueGray, kMidBlueGray};
  return colors[index % kNumColors];
}

void DrawIteratorBox(GlCanvas* canvas, Vec2 pos, Vec2 size, const Color& color,
                     const std::string& label, const std::string& time, float text_y) {
  Box box(pos, size, GlCanvas::kZValueOverlay);
  canvas->GetBatcher()->AddBox(box, color);

  std::string text = absl::StrFormat("%s: %s", label, time);

  constexpr const float kAdditionalSpaceForLine = 10.f;
  constexpr const float kLeftOffset = 5.f;

  float max_size = size[0];
  canvas->GetTextRenderer().AddTextTrailingCharsPrioritized(
      text.c_str(), pos[0] + kLeftOffset, text_y + kAdditionalSpaceForLine, GlCanvas::kZValueText,
      Color(255, 255, 255, 255), time.length(), max_size);

  constexpr const float kOffsetBelowText = kAdditionalSpaceForLine / 2.f;
  Vec2 line_from(pos[0], text_y + kOffsetBelowText);
  Vec2 line_to(pos[0] + size[0], text_y + kOffsetBelowText);
  canvas->GetBatcher()->AddLine(line_from, line_to, GlCanvas::kZValueOverlay,
                                Color(255, 255, 255, 255));
}

}  // namespace

void TimeGraph::DrawOverlay(GlCanvas* canvas, PickingMode picking_mode) {
  if (picking_mode != PickingMode::kNone || iterator_text_boxes_.size() == 0) {
    return;
  }

  std::vector<std::pair<uint64_t, const TextBox*>> boxes(iterator_text_boxes_.size());
  std::copy(iterator_text_boxes_.begin(), iterator_text_boxes_.end(), boxes.begin());

  // Sort boxes by start time.
  std::sort(boxes.begin(), boxes.end(),
            [](const std::pair<uint64_t, const TextBox*>& box_a,
               const std::pair<uint64_t, const TextBox*>& box_b) -> bool {
              return box_a.second->GetTimerInfo().start() < box_b.second->GetTimerInfo().start();
            });

  // We will need the world x coordinates for the timers multiple times, so
  // we avoid recomputing them and just cache them here.
  std::vector<float> x_coords;
  x_coords.reserve(boxes.size());

  float world_start_x = canvas->GetWorldTopLeftX();
  float world_width = canvas->GetWorldWidth();

  float world_start_y = canvas->GetWorldTopLeftY();
  float world_height = canvas->GetWorldHeight();

  double inv_time_window = 1.0 / GetTimeWindowUs();

  // Draw lines for iterators.
  for (const auto& box : boxes) {
    const TimerInfo& timer_info = box.second->GetTimerInfo();

    double start_us = GetUsFromTick(timer_info.start());
    double normalized_start = start_us * inv_time_window;
    float world_timer_x = static_cast<float>(world_start_x + normalized_start * world_width);

    Vec2 pos(world_timer_x, world_start_y);
    x_coords.push_back(pos[0]);

    canvas->GetBatcher()->AddVerticalLine(pos, -world_height, GlCanvas::kZValueOverlay,
                                          GetThreadColor(timer_info.thread_id()));
  }

  // Draw boxes with timings between iterators.
  for (size_t k = 1; k < boxes.size(); ++k) {
    Vec2 pos(x_coords[k - 1], world_start_y - world_height);
    float size_x = x_coords[k] - pos[0];
    Vec2 size(size_x, world_height);
    Color color = GetIteratorBoxColor(k - 1);

    uint64_t id_a = boxes[k - 1].first;
    uint64_t id_b = boxes[k].first;
    CHECK(iterator_functions_.find(id_a) != iterator_functions_.end());
    CHECK(iterator_functions_.find(id_b) != iterator_functions_.end());
    const std::string& label =
        GetLabelBetweenIterators(*(iterator_functions_[id_a]), *(iterator_functions_[id_b]));
    const std::string& time = GetTimeString(boxes[k - 1].second, boxes[k].second);

    // Distance from the bottom where we don't want to draw.
    float bottom_margin = layout_.GetBottomMargin();

    // The height of text is chosen such that the text of the last box drawn is
    // at pos[1] + bottom_margin (lowest possible position) and the height of
    // the box showing the overall time (see below) is at pos[1] + (world_height
    // / 2.f), corresponding to the case k == 0 in the formula for 'text_y'.
    float height_per_text = ((world_height / 2.f) - bottom_margin) /
                            static_cast<float>(iterator_text_boxes_.size() - 1);
    float text_y = pos[1] + (world_height / 2.f) - static_cast<float>(k) * height_per_text;

    DrawIteratorBox(canvas, pos, size, color, label, time, text_y);
  }

  // When we have at least 3 boxes, we also draw the total time from the first
  // to the last iterator.
  if (boxes.size() > 2) {
    size_t last_index = boxes.size() - 1;

    Vec2 pos(x_coords[0], world_start_y - world_height);
    float size_x = x_coords[last_index] - pos[0];
    Vec2 size(size_x, world_height);

    std::string time = GetTimeString(boxes[0].second, boxes[last_index].second);
    std::string label("Total");

    float text_y = pos[1] + (world_height / 2.f);

    // We do not want the overall box to add any color, so we just set alpha to
    // 0.
    const Color kColorBlackTransparent(0, 0, 0, 0);
    DrawIteratorBox(canvas, pos, size, kColorBlackTransparent, label, time, text_y);
  }
}

void TimeGraph::DrawTracks(GlCanvas* canvas, PickingMode picking_mode) {
  uint32_t num_cores = GetNumCores();
  layout_.SetNumCores(num_cores);
  scheduler_track_->SetLabel(absl::StrFormat("Scheduler (%u cores)", num_cores));
  for (auto& track : sorted_tracks_) {
    if (track->GetType() == Track::kThreadTrack) {
      auto thread_track = std::static_pointer_cast<ThreadTrack>(track);
      int32_t tid = thread_track->GetThreadId();
      if (tid == SamplingProfiler::kAllThreadsFakeTid) {
        // This is the process_track_.
        std::string process_name = GOrbitApp->GetCaptureData().process_name();
        thread_track->SetName(process_name);
        thread_track->SetLabel(process_name + " (all threads)");
      } else {
        const std::string& thread_name = GOrbitApp->GetCaptureData().GetThreadName(tid);
        track->SetName(thread_name);
        std::string track_label = absl::StrFormat("%s [%u]", thread_name, tid);
        track->SetLabel(track_label);
      }
    }

    track->Draw(canvas, picking_mode);
  }
}

std::shared_ptr<SchedulerTrack> TimeGraph::GetOrCreateSchedulerTrack() {
  ScopeLock lock(mutex_);
  std::shared_ptr<SchedulerTrack> track = scheduler_track_;
  if (track == nullptr) {
    track = std::make_shared<SchedulerTrack>(this);
    tracks_.emplace_back(track);
    scheduler_track_ = track;
  }
  return track;
}

std::shared_ptr<ThreadTrack> TimeGraph::GetOrCreateThreadTrack(int32_t tid) {
  ScopeLock lock(mutex_);
  std::shared_ptr<ThreadTrack> track = thread_tracks_[tid];
  if (track == nullptr) {
    track = std::make_shared<ThreadTrack>(this, tid);
    tracks_.emplace_back(track);
    thread_tracks_[tid] = track;
    track->SetTrackColor(GetThreadColor(tid));
  }
  return track;
}

std::shared_ptr<GpuTrack> TimeGraph::GetOrCreateGpuTrack(uint64_t timeline_hash) {
  ScopeLock lock(mutex_);
  std::shared_ptr<GpuTrack> track = gpu_tracks_[timeline_hash];
  if (track == nullptr) {
    track = std::make_shared<GpuTrack>(this, string_manager_, timeline_hash);
    std::string timeline = string_manager_->Get(timeline_hash).value_or("");
    std::string label = OrbitGl::MapGpuTimelineToTrackLabel(timeline);
    track->SetName(timeline);
    track->SetLabel(label);
    tracks_.emplace_back(track);
    gpu_tracks_[timeline_hash] = track;
  }

  return track;
}

GraphTrack* TimeGraph::GetOrCreateGraphTrack(const std::string& name) {
  ScopeLock lock(mutex_);
  std::shared_ptr<GraphTrack> track = graph_tracks_[name];
  if (track == nullptr) {
    track = std::make_shared<GraphTrack>(this, name);
    track->SetName(name);
    track->SetLabel(name);
    tracks_.emplace_back(track);
    graph_tracks_[name] = track;
  }

  return track.get();
}

AsyncTrack* TimeGraph::GetOrCreateAsyncTrack(const std::string& name) {
  ScopeLock lock(mutex_);
  std::shared_ptr<AsyncTrack> track = async_tracks_[name];
  if (track == nullptr) {
    track = std::make_shared<AsyncTrack>(this, name);
    tracks_.emplace_back(track);
    async_tracks_[name] = track;
  }

  return track.get();
}

void TimeGraph::SetThreadFilter(const std::string& a_Filter) {
  thread_filter_ = a_Filter;
  NeedsUpdate();
}

void TimeGraph::SortTracks() {
  // Get or create thread track from events' thread id.
  event_count_.clear();
  event_count_[SamplingProfiler::kAllThreadsFakeTid] =
      GOrbitApp->GetCaptureData().GetCallstackData()->GetCallstackEventsCount();
  GetOrCreateThreadTrack(SamplingProfiler::kAllThreadsFakeTid);
  for (const auto& tid_and_count :
       GOrbitApp->GetCaptureData().GetCallstackData()->GetCallstackEventsCountsPerTid()) {
    const int32_t thread_id = tid_and_count.first;
    const uint32_t count = tid_and_count.second;
    event_count_[thread_id] = count;
    GetOrCreateThreadTrack(thread_id);
  }

  // Reorder threads once every second when capturing
  if (!GOrbitApp->IsCapturing() || last_thread_reorder_.QueryMillis() > 1000.0) {
    std::vector<int32_t> sortedThreadIds;

    // Show threads with instrumented functions first
    std::vector<std::pair<int32_t, uint32_t>> sortedThreads =
        OrbitUtils::ReverseValueSort(thread_count_map_);
    for (auto& pair : sortedThreads) {
      // Track "kAllThreadsFakeTid" holds all target process sampling info, it is handled
      // separately.
      if (pair.first != SamplingProfiler::kAllThreadsFakeTid) sortedThreadIds.push_back(pair.first);
    }

    // Then show threads sorted by number of events
    std::vector<std::pair<int32_t, uint32_t>> sortedByEvents =
        OrbitUtils::ReverseValueSort(event_count_);
    for (auto& pair : sortedByEvents) {
      // Track "kAllThreadsFakeTid" holds all target process sampling info, it is handled
      // separately.
      if (pair.first == SamplingProfiler::kAllThreadsFakeTid) continue;
      if (thread_count_map_.find(pair.first) == thread_count_map_.end()) {
        sortedThreadIds.push_back(pair.first);
      }
    }

    // Filter thread ids if needed
    if (!thread_filter_.empty()) {
      std::vector<std::string> filters = absl::StrSplit(thread_filter_, ' ');
      std::vector<int32_t> filteredThreadIds;
      for (int32_t tid : sortedThreadIds) {
        std::shared_ptr<ThreadTrack> track = GetOrCreateThreadTrack(tid);

        for (auto& filter : filters) {
          if (track && absl::StrContains(track->GetName(), filter)) {
            filteredThreadIds.push_back(tid);
          }
        }
      }
      sortedThreadIds = filteredThreadIds;
    }

    sorted_tracks_.clear();

    // Scheduler Track.
    if (!scheduler_track_->IsEmpty()) {
      sorted_tracks_.emplace_back(scheduler_track_);
    }

    // Gpu Tracks.
    for (const auto& timeline_and_track : gpu_tracks_) {
      sorted_tracks_.emplace_back(timeline_and_track.second);
    }

    // Graph Tracks.
    for (const auto& graph_track : graph_tracks_) {
      sorted_tracks_.emplace_back(graph_track.second);
    }

    // Async Tracks.
    for (const auto& async_track : async_tracks_) {
      sorted_tracks_.emplace_back(async_track.second);
    }

    // Process Track.
    if (!process_track_->IsEmpty()) {
      sorted_tracks_.emplace_back(process_track_);
    }

    // Thread Tracks.
    for (auto thread_id : sortedThreadIds) {
      std::shared_ptr<ThreadTrack> track = GetOrCreateThreadTrack(thread_id);
      if (!track->IsEmpty()) {
        sorted_tracks_.emplace_back(track);
      }
    }

    last_thread_reorder_.Reset();
  }
}

void TimeGraph::SelectAndZoom(const TextBox* text_box) {
  CHECK(text_box);
  Zoom(text_box);
  Select(text_box);
}

void TimeGraph::JumpToNeighborBox(const TextBox* from, JumpDirection jump_direction,
                                  JumpScope jump_scope) {
  const TextBox* goal = nullptr;
  if (!from) {
    return;
  }
  auto function_address = from->GetTimerInfo().function_address();
  auto current_time = from->GetTimerInfo().end();
  auto thread_id = from->GetTimerInfo().thread_id();
  if (jump_direction == JumpDirection::kPrevious) {
    switch (jump_scope) {
      case JumpScope::kSameDepth:
        goal = FindPrevious(from);
        break;
      case JumpScope::kSameFunction:
        goal = FindPreviousFunctionCall(function_address, current_time);
        break;
      case JumpScope::kSameThreadSameFunction:
        goal = FindPreviousFunctionCall(function_address, current_time, thread_id);
        break;
      default:
        // Other choices are not implemented.
        CHECK(false);
        break;
    }
  }
  if (jump_direction == JumpDirection::kNext) {
    switch (jump_scope) {
      case JumpScope::kSameDepth:
        goal = FindNext(from);
        break;
      case JumpScope::kSameFunction:
        goal = FindNextFunctionCall(function_address, current_time);
        break;
      case JumpScope::kSameThreadSameFunction:
        goal = FindNextFunctionCall(function_address, current_time, thread_id);
        break;
      default:
        CHECK(false);
        break;
    }
  }
  if (jump_direction == JumpDirection::kTop) {
    goal = FindTop(from);
  }
  if (jump_direction == JumpDirection::kDown) {
    goal = FindDown(from);
  }
  if (goal) {
    Select(goal);
  }
}

const TextBox* TimeGraph::FindPrevious(const TextBox* from) {
  CHECK(from);
  const TimerInfo& timer_info = from->GetTimerInfo();
  if (timer_info.type() == TimerInfo::kGpuActivity) {
    return GetOrCreateGpuTrack(timer_info.timeline_hash())->GetLeft(from);
  } else {
    return GetOrCreateThreadTrack(timer_info.thread_id())->GetLeft(from);
  }
  return nullptr;
}

const TextBox* TimeGraph::FindNext(const TextBox* from) {
  CHECK(from);
  const TimerInfo& timer_info = from->GetTimerInfo();
  if (timer_info.type() == TimerInfo::kGpuActivity) {
    return GetOrCreateGpuTrack(timer_info.timeline_hash())->GetRight(from);
  } else {
    return GetOrCreateThreadTrack(timer_info.thread_id())->GetRight(from);
  }
  return nullptr;
}

const TextBox* TimeGraph::FindTop(const TextBox* from) {
  CHECK(from);
  const TimerInfo& timer_info = from->GetTimerInfo();
  if (timer_info.type() == TimerInfo::kGpuActivity) {
    return GetOrCreateGpuTrack(timer_info.timeline_hash())->GetUp(from);
  } else {
    return GetOrCreateThreadTrack(timer_info.thread_id())->GetUp(from);
  }
  return nullptr;
}

const TextBox* TimeGraph::FindDown(const TextBox* from) {
  CHECK(from);
  const TimerInfo& timer_info = from->GetTimerInfo();
  if (timer_info.type() == TimerInfo::kGpuActivity) {
    return GetOrCreateGpuTrack(timer_info.timeline_hash())->GetDown(from);
  } else {
    return GetOrCreateThreadTrack(timer_info.thread_id())->GetDown(from);
  }
  return nullptr;
}

void TimeGraph::DrawText(GlCanvas* canvas) {
  if (draw_text_) {
    text_renderer_static_.Display(canvas->GetBatcher());
  }
}

bool TimeGraph::IsFullyVisible(uint64_t min, uint64_t max) const {
  double start = TicksToMicroseconds(capture_min_timestamp_, min);
  double end = TicksToMicroseconds(capture_min_timestamp_, max);

  return start > min_time_us_ && end < max_time_us_;
}

bool TimeGraph::IsPartlyVisible(uint64_t min, uint64_t max) const {
  double start = TicksToMicroseconds(capture_min_timestamp_, min);
  double end = TicksToMicroseconds(capture_min_timestamp_, max);

  double startUs = min_time_us_;

  if (startUs > end || max_time_us_ < start) {
    return false;
  }

  return true;
}

bool TimeGraph::IsVisible(VisibilityType vis_type, uint64_t min, uint64_t max) const {
  switch (vis_type) {
    case VisibilityType::kPartlyVisible:
      return IsPartlyVisible(min, max);
    case VisibilityType::kFullyVisible:
      return IsFullyVisible(min, max);
    default:
      return false;
  }
}
