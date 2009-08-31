// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/render_thread.h"

#include <algorithm>
#include <vector>

#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/shared_memory.h"
#include "base/stats_table.h"
#include "base/thread_local.h"
#include "chrome/common/appcache/appcache_dispatcher.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/db_message_filter.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/renderer_preferences.h"
#include "chrome/common/url_constants.h"
#include "chrome/plugin/npobject_util.h"
// TODO(port)
#if defined(OS_WIN)
#include "chrome/plugin/plugin_channel.h"
#else
#include "base/scoped_handle.h"
#include "chrome/plugin/plugin_channel_base.h"
#endif
#include "chrome/renderer/devtools_agent_filter.h"
#include "chrome/renderer/extension_groups.h"
#include "chrome/renderer/extensions/event_bindings.h"
#include "chrome/renderer/extensions/extension_process_bindings.h"
#include "chrome/renderer/extensions/js_only_v8_extensions.h"
#include "chrome/renderer/extensions/renderer_extension_bindings.h"
#include "chrome/renderer/external_extension.h"
#include "chrome/renderer/loadtimes_extension_bindings.h"
#include "chrome/renderer/net/render_dns_master.h"
#include "chrome/renderer/render_process.h"
#include "chrome/renderer/render_view.h"
#include "chrome/renderer/renderer_webkitclient_impl.h"
#include "chrome/renderer/user_script_slave.h"
#include "webkit/api/public/WebColor.h"
#include "webkit/api/public/WebCache.h"
#include "webkit/api/public/WebKit.h"
#include "webkit/api/public/WebString.h"
#include "webkit/extensions/v8/benchmarking_extension.h"
#include "webkit/extensions/v8/gears_extension.h"
#include "webkit/extensions/v8/interval_extension.h"
#include "webkit/extensions/v8/playback_extension.h"

#if defined(OS_WIN)
#include <windows.h>
#include <objbase.h>
#endif

using WebKit::WebCache;
using WebKit::WebString;

namespace {
static const unsigned int kCacheStatsDelayMS = 2000 /* milliseconds */;
static const double kInitialIdleHandlerDelayS = 1.0 /* seconds */;

static base::LazyInstance<base::ThreadLocalPointer<RenderThread> > lazy_tls(
    base::LINKER_INITIALIZED);

#if defined(OS_POSIX)
class SuicideOnChannelErrorFilter : public IPC::ChannelProxy::MessageFilter {
  void OnChannelError() {
    // On POSIX, at least, one can install an unload handler which loops
    // forever and leave behind a renderer process which eats 100% CPU forever.
    //
    // This is because the terminate signals (ViewMsg_ShouldClose and the error
    // from the IPC channel) are routed to the main message loop but never
    // processed (because that message loop is stuck in V8).
    //
    // One could make the browser SIGKILL the renderers, but that leaves open a
    // large window where a browser failure (or a user, manually terminating
    // the browser because "it's stuck") will leave behind a process eating all
    // the CPU.
    //
    // So, we install a filter on the channel so that we can process this event
    // here and kill the process.
    _exit(0);
  }
};
#endif
}  // namespace

// When we run plugins in process, we actually run them on the render thread,
// which means that we need to make the render thread pump UI events.
RenderThread::RenderThread() {
  Init();
}

RenderThread::RenderThread(const std::string& channel_name)
    : ChildThread(channel_name) {
  Init();
}

void RenderThread::Init() {
  lazy_tls.Pointer()->Set(this);
#if defined(OS_WIN)
  // If you are running plugins in this thread you need COM active but in
  // the normal case you don't.
  if (RenderProcess::InProcessPlugins())
    CoInitialize(0);
#endif

  plugin_refresh_allowed_ = true;
  cache_stats_task_pending_ = false;
  widget_count_ = 0;
  hidden_widget_count_ = 0;
  idle_notification_delay_in_s_ = kInitialIdleHandlerDelayS;
  task_factory_.reset(new ScopedRunnableMethodFactory<RenderThread>(this));

  visited_link_slave_.reset(new VisitedLinkSlave());
  user_script_slave_.reset(new UserScriptSlave());
  dns_master_.reset(new RenderDnsMaster());
  histogram_snapshots_.reset(new RendererHistogramSnapshots());
  appcache_dispatcher_.reset(new AppCacheDispatcher(this));
  devtools_agent_filter_ = new DevToolsAgentFilter();
  AddFilter(devtools_agent_filter_.get());
  db_message_filter_ = new DBMessageFilter();
  AddFilter(db_message_filter_.get());

#if defined(OS_POSIX)
  suicide_on_channel_error_filter_ = new SuicideOnChannelErrorFilter;
  AddFilter(suicide_on_channel_error_filter_.get());
#endif
}

RenderThread::~RenderThread() {
  // Shutdown in reverse of the initialization order.
  RemoveFilter(devtools_agent_filter_.get());
  RemoveFilter(db_message_filter_.get());
  db_message_filter_ = NULL;
  if (webkit_client_.get())
    WebKit::shutdown();

  lazy_tls.Pointer()->Set(NULL);

  // TODO(port)
#if defined(OS_WIN)
  // Clean up plugin channels before this thread goes away.
  PluginChannelBase::CleanupChannels();
  // Don't call COM if the renderer is in the sandbox.
  if (RenderProcess::InProcessPlugins())
    CoUninitialize();
#endif
}

RenderThread* RenderThread::current() {
  return lazy_tls.Pointer()->Get();
}

void RenderThread::AddFilter(IPC::ChannelProxy::MessageFilter* filter) {
  channel()->AddFilter(filter);
}

void RenderThread::RemoveFilter(IPC::ChannelProxy::MessageFilter* filter) {
  channel()->RemoveFilter(filter);
}

void RenderThread::WidgetHidden() {
  DCHECK(hidden_widget_count_ < widget_count_);
  hidden_widget_count_++ ;
  if (widget_count_ && hidden_widget_count_ == widget_count_) {
    // Reset the delay.
    idle_notification_delay_in_s_ = kInitialIdleHandlerDelayS;

    // Schedule the IdleHandler to wakeup in a bit.
    MessageLoop::current()->PostDelayedTask(FROM_HERE,
        task_factory_->NewRunnableMethod(&RenderThread::IdleHandler),
        static_cast<int64>(floor(idle_notification_delay_in_s_)) * 1000);
  }
}

void RenderThread::WidgetRestored() {
  DCHECK(hidden_widget_count_ > 0);
  hidden_widget_count_--;

  // Note: we may have a timer pending to call the IdleHandler (see the
  // WidgetHidden() code).  But we don't bother to cancel it as it is
  // benign and won't do anything if the tab is un-hidden when it is
  // called.
}

void RenderThread::Resolve(const char* name, size_t length) {
  return dns_master_->Resolve(name, length);
}

void RenderThread::SendHistograms(int sequence_number) {
  return histogram_snapshots_->SendHistograms(sequence_number);
}

void RenderThread::OnUpdateVisitedLinks(base::SharedMemoryHandle table) {
  DCHECK(base::SharedMemory::IsHandleValid(table)) << "Bad table handle";
  visited_link_slave_->Init(table);
}

void RenderThread::OnAddVisitedLinks(
    const VisitedLinkSlave::Fingerprints& fingerprints) {
  for (size_t i = 0; i < fingerprints.size(); ++i)
    WebView::UpdateVisitedLinkState(fingerprints[i]);
}

void RenderThread::OnResetVisitedLinks() {
  WebView::ResetVisitedLinkState();
}

void RenderThread::OnUpdateUserScripts(
    base::SharedMemoryHandle scripts) {
  DCHECK(base::SharedMemory::IsHandleValid(scripts)) << "Bad scripts handle";
  user_script_slave_->UpdateScripts(scripts);
}

void RenderThread::OnSetExtensionFunctionNames(
    const std::vector<std::string>& names) {
  ExtensionProcessBindings::SetFunctionNames(names);
}

void RenderThread::OnPageActionsUpdated(
    const std::string& extension_id,
    const std::vector<std::string>& page_actions) {
  ExtensionProcessBindings::SetPageActions(extension_id, page_actions);
}

void RenderThread::OnExtensionSetAPIPermissions(
    const std::string& extension_id,
    const std::vector<std::string>& permissions) {
  ExtensionProcessBindings::SetAPIPermissions(extension_id, permissions);
}

void RenderThread::OnExtensionSetHostPermissions(
    const GURL& extension_url, const std::vector<URLPattern>& permissions) {
  ExtensionProcessBindings::SetHostPermissions(extension_url, permissions);
}

void RenderThread::OnControlMessageReceived(const IPC::Message& msg) {
  // App cache messages are handled by a delegate.
  if (appcache_dispatcher_->OnMessageReceived(msg))
    return;

  IPC_BEGIN_MESSAGE_MAP(RenderThread, msg)
    IPC_MESSAGE_HANDLER(ViewMsg_VisitedLink_NewTable, OnUpdateVisitedLinks)
    IPC_MESSAGE_HANDLER(ViewMsg_VisitedLink_Add, OnAddVisitedLinks)
    IPC_MESSAGE_HANDLER(ViewMsg_VisitedLink_Reset, OnResetVisitedLinks)
    IPC_MESSAGE_HANDLER(ViewMsg_SetNextPageID, OnSetNextPageID)
    IPC_MESSAGE_HANDLER(ViewMsg_SetCSSColors, OnSetCSSColors)
    // TODO(port): removed from render_messages_internal.h;
    // is there a new non-windows message I should add here?
    IPC_MESSAGE_HANDLER(ViewMsg_New, OnCreateNewView)
    IPC_MESSAGE_HANDLER(ViewMsg_SetCacheCapacities, OnSetCacheCapacities)
    IPC_MESSAGE_HANDLER(ViewMsg_GetRendererHistograms,
                        OnGetRendererHistograms)
    IPC_MESSAGE_HANDLER(ViewMsg_GetCacheResourceStats,
                        OnGetCacheResourceStats)
    IPC_MESSAGE_HANDLER(ViewMsg_UserScripts_UpdatedScripts,
                        OnUpdateUserScripts)
    // TODO(rafaelw): create an ExtensionDispatcher that handles extension
    // messages seperates their handling from the RenderThread.
    IPC_MESSAGE_HANDLER(ViewMsg_ExtensionMessageInvoke,
                        OnExtensionMessageInvoke)
    IPC_MESSAGE_HANDLER(ViewMsg_Extension_SetFunctionNames,
                        OnSetExtensionFunctionNames)
    IPC_MESSAGE_HANDLER(ViewMsg_PurgePluginListCache,
                        OnPurgePluginListCache)
    IPC_MESSAGE_HANDLER(ViewMsg_Extension_UpdatePageActions,
                        OnPageActionsUpdated)
    IPC_MESSAGE_HANDLER(ViewMsg_Extension_SetAPIPermissions,
                        OnExtensionSetAPIPermissions)
    IPC_MESSAGE_HANDLER(ViewMsg_Extension_SetHostPermissions,
                        OnExtensionSetHostPermissions)
  IPC_END_MESSAGE_MAP()
}

void RenderThread::OnSetNextPageID(int32 next_page_id) {
  // This should only be called at process initialization time, so we shouldn't
  // have to worry about thread-safety.
  RenderView::SetNextPageID(next_page_id);
}

// Called when to register CSS Color name->system color mappings.
// We update the colors one by one and then tell WebKit to refresh all render
// views.
void RenderThread::OnSetCSSColors(
    const std::vector<CSSColors::CSSColorMapping>& colors) {

  size_t num_colors = colors.size();
  scoped_array<WebKit::WebColorName> color_names(
      new WebKit::WebColorName[num_colors]);
  scoped_array<WebKit::WebColor> web_colors(new WebKit::WebColor[num_colors]);
  size_t i = 0;
  for (std::vector<CSSColors::CSSColorMapping>::const_iterator it =
          colors.begin();
       it != colors.end();
       ++it, ++i) {
    color_names[i] = it->first;
    web_colors[i] = it->second;
  }
  WebKit::setNamedColors(color_names.get(), web_colors.get(), num_colors);
}

void RenderThread::OnCreateNewView(gfx::NativeViewId parent_hwnd,
                                   ModalDialogEvent modal_dialog_event,
                                   const RendererPreferences& renderer_prefs,
                                   const WebPreferences& webkit_prefs,
                                   int32 view_id) {
  EnsureWebKitInitialized();

  // When bringing in render_view, also bring in webkit's glue and jsbindings.
  base::WaitableEvent* waitable_event = new base::WaitableEvent(
#if defined(OS_WIN)
      modal_dialog_event.event);
#else
      true, false);
#endif

  RenderView::Create(
      this, parent_hwnd, waitable_event, MSG_ROUTING_NONE, renderer_prefs,
      webkit_prefs, new SharedRenderViewCounter(0), view_id);
}

void RenderThread::OnSetCacheCapacities(size_t min_dead_capacity,
                                        size_t max_dead_capacity,
                                        size_t capacity) {
  EnsureWebKitInitialized();
  WebCache::setCapacities(
      min_dead_capacity, max_dead_capacity, capacity);
}

void RenderThread::OnGetCacheResourceStats() {
  EnsureWebKitInitialized();
  WebCache::ResourceTypeStats stats;
  WebCache::getResourceTypeStats(&stats);
  Send(new ViewHostMsg_ResourceTypeStats(stats));
}

void RenderThread::OnGetRendererHistograms(int sequence_number) {
  SendHistograms(sequence_number);
}

void RenderThread::InformHostOfCacheStats() {
  EnsureWebKitInitialized();
  WebCache::UsageStats stats;
  WebCache::getUsageStats(&stats);
  Send(new ViewHostMsg_UpdatedCacheStats(stats));
  cache_stats_task_pending_ = false;
}

void RenderThread::InformHostOfCacheStatsLater() {
  // Rate limit informing the host of our cache stats.
  if (cache_stats_task_pending_)
    return;

  cache_stats_task_pending_ = true;
  MessageLoop::current()->PostDelayedTask(FROM_HERE,
      task_factory_->NewRunnableMethod(
          &RenderThread::InformHostOfCacheStats),
      kCacheStatsDelayMS);
}

void RenderThread::CloseIdleConnections() {
  Send(new ViewHostMsg_CloseIdleConnections());
}

void RenderThread::SetCacheMode(bool enabled) {
  Send(new ViewHostMsg_SetCacheMode(enabled));
}

static void* CreateHistogram(
    const char *name, int min, int max, size_t buckets) {
  Histogram* histogram = new Histogram(name, min, max, buckets);
  if (histogram) {
    histogram->SetFlags(kUmaTargetedHistogramFlag);
  }
  return histogram;
}

static void AddHistogramSample(void* hist, int sample) {
  Histogram* histogram = static_cast<Histogram *>(hist);
  histogram->Add(sample);
}

void RenderThread::EnsureWebKitInitialized() {
  if (webkit_client_.get())
    return;

  v8::V8::SetCounterFunction(StatsTable::FindLocation);
  v8::V8::SetCreateHistogramFunction(CreateHistogram);
  v8::V8::SetAddHistogramSampleFunction(AddHistogramSample);

  webkit_client_.reset(new RendererWebKitClientImpl);
  WebKit::initialize(webkit_client_.get());

  WebKit::enableV8SingleThreadMode();

  // chrome: pages should not be accessible by normal content, and should
  // also be unable to script anything but themselves (to help limit the damage
  // that a corrupt chrome: page could cause).
  WebString chrome_ui_scheme(ASCIIToUTF16(chrome::kChromeUIScheme));
  WebKit::registerURLSchemeAsLocal(chrome_ui_scheme);
  WebKit::registerURLSchemeAsNoAccess(chrome_ui_scheme);

  // print: pages should be not accessible by normal context.
  WebString print_ui_scheme(ASCIIToUTF16(chrome::kPrintScheme));
  WebKit::registerURLSchemeAsLocal(print_ui_scheme);
  WebKit::registerURLSchemeAsNoAccess(print_ui_scheme);

#if defined(OS_WIN)
  // We don't yet support Gears on non-Windows, so don't tell pages that we do.
  WebKit::registerExtension(extensions_v8::GearsExtension::Get());
#endif
  WebKit::registerExtension(extensions_v8::IntervalExtension::Get());
  WebKit::registerExtension(extensions_v8::LoadTimesExtension::Get());
  WebKit::registerExtension(extensions_v8::ExternalExtension::Get());

  const WebKit::WebString kExtensionScheme =
      WebKit::WebString::fromUTF8(chrome::kExtensionScheme);

  WebKit::registerExtension(ExtensionProcessBindings::Get(), kExtensionScheme);

  WebKit::registerExtension(BaseJsV8Extension::Get(),
                            EXTENSION_GROUP_CONTENT_SCRIPTS);
  WebKit::registerExtension(BaseJsV8Extension::Get(), kExtensionScheme);
  WebKit::registerExtension(JsonSchemaJsV8Extension::Get(),
                            EXTENSION_GROUP_CONTENT_SCRIPTS);
  WebKit::registerExtension(JsonSchemaJsV8Extension::Get(), kExtensionScheme);
  WebKit::registerExtension(EventBindings::Get(),
                            EXTENSION_GROUP_CONTENT_SCRIPTS);
  WebKit::registerExtension(EventBindings::Get(), kExtensionScheme);
  WebKit::registerExtension(RendererExtensionBindings::Get(),
                            EXTENSION_GROUP_CONTENT_SCRIPTS);
  WebKit::registerExtension(RendererExtensionBindings::Get(), kExtensionScheme);
  WebKit::registerExtension(ExtensionApiTestV8Extension::Get(),
                            kExtensionScheme);
  WebKit::registerExtension(ExtensionApiTestV8Extension::Get(),
                            EXTENSION_GROUP_CONTENT_SCRIPTS);

  const CommandLine& command_line = *CommandLine::ForCurrentProcess();

  if (command_line.HasSwitch(switches::kEnableBenchmarking))
    WebKit::registerExtension(extensions_v8::BenchmarkingExtension::Get());

  if (command_line.HasSwitch(switches::kPlaybackMode) ||
      command_line.HasSwitch(switches::kRecordMode) ||
      command_line.HasSwitch(switches::kNoJsRandomness)) {
    WebKit::registerExtension(extensions_v8::PlaybackExtension::Get());
  }

  if (RenderProcess::current()->initialized_media_library())
    WebKit::enableMediaPlayer();
}

void RenderThread::IdleHandler() {
  // It is possible that the timer was set while the widgets were idle,
  // but that they are no longer idle.  If so, just return.
  if (!widget_count_ || hidden_widget_count_ < widget_count_)
    return;

  if (v8::V8::IsDead())
    return;

  LOG(INFO) << "RenderThread calling v8 IdleNotification for " << this;

  // When V8::IdleNotification returns true, it means that it has cleaned up
  // as much as it can.  There is no point in continuing to call it.
  if (!v8::V8::IdleNotification(false)) {
    // Dampen the delay using the algorithm:
    //    delay = delay + 1 / (delay + 2)
    // Using floor(delay) has a dampening effect such as:
    //    1s, 1, 1, 2, 2, 2, 2, 3, 3, ...
    idle_notification_delay_in_s_ +=
        1.0 / (idle_notification_delay_in_s_ + 2.0);

    // Schedule the next timer.
    MessageLoop::current()->PostDelayedTask(FROM_HERE,
        task_factory_->NewRunnableMethod(&RenderThread::IdleHandler),
        static_cast<int64>(floor(idle_notification_delay_in_s_)) * 1000);
  }
}

void RenderThread::OnExtensionMessageInvoke(const std::string& function_name,
                                            const ListValue& args) {
  RendererExtensionBindings::Invoke(function_name, args, NULL);
}

void RenderThread::OnPurgePluginListCache() {
  // The call below will cause a GetPlugins call with refresh=true, but at this
  // point we already know that the browser has refreshed its list, so disable
  // refresh temporarily to prevent each renderer process causing the list to be
  // regenerated.
  plugin_refresh_allowed_ = false;
  WebKit::resetPluginCache();
  plugin_refresh_allowed_ = true;
}
