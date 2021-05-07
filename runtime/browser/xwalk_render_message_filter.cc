// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright (c) 2014 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/browser/xwalk_render_message_filter.h"

#if defined(OS_ANDROID)
#include "xwalk/runtime/browser/android/xwalk_contents_client_bridge.h"
#include "xwalk/runtime/browser/android/xwalk_contents_io_thread_client.h"
#include "xwalk/runtime/common/android/xwalk_render_view_messages.h"
#endif

#include "xwalk/runtime/common/xwalk_common_messages.h"
#include "xwalk/runtime/browser/runtime_platform_util.h"

using content::BrowserThread;

namespace xwalk {

XWalkRenderMessageFilter::XWalkRenderMessageFilter()
    : BrowserMessageFilter(ViewMsgStart) {
}

#if defined(OS_ANDROID)
XWalkRenderMessageFilter::XWalkRenderMessageFilter(int process_id)
    : BrowserMessageFilter(AndroidWebViewMsgStart),
      process_id_(process_id) {
}

void XWalkRenderMessageFilter::OverrideThreadForMessage(
                                                        const IPC::Message& message,
                                                        BrowserThread::ID* thread) {
  if (message.type() == XWalkViewHostMsg_ShouldOverrideUrlLoading::ID ||
      message.type() == XWalkViewHostMsg_WillSendRequest::ID) {
    *thread = BrowserThread::UI;
  }
}
#endif

bool XWalkRenderMessageFilter::OnMessageReceived(
                                                 const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(XWalkRenderMessageFilter, message)
    IPC_MESSAGE_HANDLER(ViewMsg_OpenLinkExternal, OnOpenLinkExternal)
#if defined(OS_ANDROID)
    IPC_MESSAGE_HANDLER(XWalkViewHostMsg_SubFrameCreated, OnSubFrameCreated)
    IPC_MESSAGE_HANDLER(XWalkViewHostMsg_ShouldOverrideUrlLoading,
                        OnShouldOverrideUrlLoading)
    IPC_MESSAGE_HANDLER(XWalkViewHostMsg_WillSendRequest,
                        OnWillSendRequest)
#endif
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()

  return handled;
}

void XWalkRenderMessageFilter::OnOpenLinkExternal(const GURL& url) {
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "OpenLinkExternal: " << url.spec();
#endif
  platform_util::OpenExternal(url);
}

#if defined(OS_ANDROID)
void XWalkRenderMessageFilter::OnSubFrameCreated(int parent_render_frame_id,
                                                 int child_render_frame_id) {
  XWalkContentsIoThreadClient::SubFrameCreated(
                                               process_id_,
                                               parent_render_frame_id, child_render_frame_id);
}

void XWalkRenderMessageFilter::OnShouldOverrideUrlLoading(int render_frame_id,
                                                          const base::string16& url,
                                                          bool has_user_gesture,
                                                          bool is_redirect,
                                                          bool is_main_frame,
                                                          bool* ignore_navigation) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  *ignore_navigation = false;
  XWalkContentsClientBridge* client =
      XWalkContentsClientBridge::FromRenderFrameID(process_id_, render_frame_id);
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << __func__ << " pocess_id=" << process_id_ << " frame_id="
               << render_frame_id;
#endif
  if (client) {
    *ignore_navigation = client->ShouldOverrideUrlLoading(
                                                          url,
                                                          has_user_gesture, is_redirect,
                                                          is_main_frame);
  } else {
#if TENTA_LOG_ENABLE == 1
    LOG(WARNING) << "Failed to find the associated render view host pocess_id="
                    << process_id_ << " frame_id=" << render_frame_id
                    << " for url: " << url;
#endif
  }
}

void XWalkRenderMessageFilter::OnWillSendRequest(int render_frame_id, const std::string& doc_url,
                                                 const std::string& url,
                                                 ui::PageTransition transition_type,
                                                 int navigation_type,
                                                 bool isMainFrame,
                                                 std::string* new_url,
                                                 bool* did_overwrite) {
  // TODO(iotto) check new_url/did_overwrite for null
  *did_overwrite = false;
  XWalkContentsClientBridge* client =
      XWalkContentsClientBridge::FromRenderFrameID(process_id_, render_frame_id);

  if ( client ) {
    *did_overwrite = client->RewriteUrlIfNeeded(doc_url, url, transition_type, navigation_type, isMainFrame, new_url);
  } else {
#if TENTA_LOG_ENABLE == 1
    LOG(WARNING) << "Failed to find the associated render view host pocess_id="
                    << process_id_ << " frame_id=" << render_frame_id
                    << " for url: " << url;
#endif
  }
}
#endif

}  // namespace xwalk
