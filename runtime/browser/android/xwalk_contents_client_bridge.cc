// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/browser/android/xwalk_contents_client_bridge.h"

#include <string>

#include "base/android/jni_android.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/guid.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/favicon_status.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/file_chooser_file_info.h"
#include "content/public/common/file_chooser_params.h"
#include "content/public/common/notification_resources.h"
#include "content/public/common/platform_notification_data.h"
//#include "grit/components_strings.h"
#include "components/strings/grit/components_strings.h"
#include "jni/XWalkContentsClientBridge_jni.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/android/java_bitmap.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "url/gurl.h"
#include "net/base/escape.h"
#include "net/cert/cert_database.h"
#include "net/cert/x509_certificate.h"
//#include "net/ssl/openssl_client_key_store.h"
#include "net/ssl/ssl_platform_key_android.h"
#include "net/ssl/ssl_private_key.h"
#include "meta_logging.h"

using base::android::AttachCurrentThread;
using base::android::ConvertJavaStringToUTF16;
using base::android::ConvertUTF8ToJavaString;
using base::android::ConvertUTF16ToJavaString;
using base::android::JavaRef;
using base::android::ScopedJavaGlobalRef;
using base::android::ScopedJavaLocalRef;
using base::android::JavaParamRef;
//using base::ScopedPtrHashMap;
using content::BrowserThread;
using content::FileChooserParams;
using content::RenderViewHost;
using content::WebContents;

namespace xwalk {
namespace {
const void* const kXWalkContentsClientBridge = &kXWalkContentsClientBridge;

// This class is invented so that the UserData registry that we inject the
// XWalkContentsClientBridge object does not own and destroy it.
class UserData : public base::SupportsUserData::Data {
 public:
  static XWalkContentsClientBridge* GetContents(
      content::WebContents* web_contents) {
    if (!web_contents) {
      TENTA_LOG_NET(ERROR) << __func__ << " null_webcontents";
      return NULL;
    }
    UserData* data = static_cast<UserData*>(
        web_contents->GetUserData(kXWalkContentsClientBridge));
    return data ? data->contents_ : NULL;
  }

  explicit UserData(XWalkContentsClientBridge* ptr) : contents_(ptr) {}

 private:
  XWalkContentsClientBridge* contents_;

  DISALLOW_COPY_AND_ASSIGN(UserData);
};

} // namesapce

namespace {

// Must be called on the I/O thread to record a client certificate
// and its private key in the OpenSSLClientKeyStore.
//void RecordClientCertificateKey(
//    const scoped_refptr<net::X509Certificate>& client_cert,
//    scoped_refptr<net::SSLPrivateKey> private_key) {
//  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
//  net::OpenSSLClientKeyStore::GetInstance()->RecordClientCertPrivateKey(
//      client_cert.get(), private_key.get());
//}

void NotifyClientCertificatesChanged() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  net::CertDatabase::GetInstance()->OnAndroidKeyStoreChanged();
}

}  // namespace

// static
void XWalkContentsClientBridge::Associate(WebContents* web_contents, XWalkContentsClientBridge* handler) {
  web_contents->SetUserData(kXWalkContentsClientBridge, base::MakeUnique<UserData>(handler));
}

// static
XWalkContentsClientBridge* XWalkContentsClientBridge::FromWebContents(WebContents* web_contents) {
  return UserData::GetContents(web_contents);
}

// static
XWalkContentsClientBridge* XWalkContentsClientBridge::FromWebContentsGetter(
    const content::ResourceRequestInfo::WebContentsGetter& web_contents_getter) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  WebContents* web_contents = web_contents_getter.Run();
  return UserData::GetContents(web_contents);
}

// static
XWalkContentsClientBridge* XWalkContentsClientBridge::FromID(int render_process_id, int render_frame_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(render_process_id, render_frame_id);
  content::WebContents* web_contents = content::WebContents::FromRenderFrameHost(rfh);
  return UserData::GetContents(web_contents);
}

// static
XWalkContentsClientBridge* XWalkContentsClientBridge::FromRenderViewID(int render_process_id, int render_view_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::RenderViewHost* rvh = content::RenderViewHost::FromID(render_process_id, render_view_id);
  if (!rvh)
    return NULL;
  content::WebContents* web_contents = content::WebContents::FromRenderViewHost(rvh);
  return UserData::GetContents(web_contents);
}

// static
XWalkContentsClientBridge* XWalkContentsClientBridge::FromRenderFrameID(int render_process_id, int render_frame_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(render_process_id, render_frame_id);
  if (!rfh)
    return NULL;
  content::WebContents* web_contents = content::WebContents::FromRenderFrameHost(rfh);
  return UserData::GetContents(web_contents);
}

// static
XWalkContentsClientBridge*
XWalkContentsClientBridge::FromRenderFrameHost(content::RenderFrameHost* render_frame_host) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::WebContents* web_contents = content::WebContents::FromRenderFrameHost(render_frame_host);
  return UserData::GetContents(web_contents);
}

//namespace {
//
//int g_next_notification_id_ = 1;
//
//std::map<int, std::unique_ptr<content::DesktopNotificationDelegate>>
//    g_notification_map_;
//
//}  // namespace


XWalkContentsClientBridge::XWalkContentsClientBridge(
    JNIEnv* env, const base::android::JavaParamRef<jobject>& obj,
    content::WebContents* web_contents)
    : java_ref_(env, obj) {
  DCHECK(obj);
  Java_XWalkContentsClientBridge_setNativeContentsClientBridge(env, obj, reinterpret_cast<intptr_t>(this));
  icon_helper_.reset(new XWalkIconHelper(web_contents));
  if (icon_helper_.get() != nullptr) {
    icon_helper_->SetListener(this);
  }
}

XWalkContentsClientBridge::~XWalkContentsClientBridge() {
  if (icon_helper_.get() != nullptr)
    icon_helper_->SetListener(nullptr);

  JNIEnv* env = AttachCurrentThread();

  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;
  // Clear the weak reference from the java peer to the native object since
  // it is possible that java object lifetime can exceed the XWalkViewContents.
  Java_XWalkContentsClientBridge_setNativeContentsClientBridge(
      env, obj, 0);
}

void XWalkContentsClientBridge::AllowCertificateError(
    int cert_error,
    net::X509Certificate* cert,
    const GURL& request_url,
    const base::Callback<void(content::CertificateRequestResultType)>& callback) {

  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  JNIEnv* env = AttachCurrentThread();

  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  std::string der_string;
  if (!net::X509Certificate::GetDEREncoded(cert->os_cert_handle(),
      &der_string))
    return;
  ScopedJavaLocalRef<jbyteArray> jcert = base::android::ToJavaByteArray(
      env,
      reinterpret_cast<const uint8_t*>(der_string.data()),
      der_string.length());
  ScopedJavaLocalRef<jstring> jurl(ConvertUTF8ToJavaString(
      env, request_url.spec()));
  // We need to add the callback before making the call to java side,
  // as it may do a synchronous callback prior to returning.
  int request_id = pending_cert_error_callbacks_.Add(
      base::MakeUnique<CertErrorCallback>(callback));
  bool cancel_request = !Java_XWalkContentsClientBridge_allowCertificateError(
      env, obj, cert_error, jcert, jurl, request_id);
  // if the request is cancelled, then cancel the stored callback
  if (cancel_request) {
    pending_cert_error_callbacks_.Remove(request_id);
    callback.Run(content::CertificateRequestResultType::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
  }
}

void XWalkContentsClientBridge::ProceedSslError(JNIEnv* env, jobject obj,
                                                jboolean proceed, jint id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  CertErrorCallback* callback = pending_cert_error_callbacks_.Lookup(id);
  if (!callback || callback->is_null()) {
    LOG(WARNING) << "Ignoring unexpected ssl error proceed callback";
    return;
  }
  // TODO(iotto) rewrite java to allow CERTIFICATE_REQUEST_RESULT_TYPE_CANCEL
  if ( proceed ) {
    callback->Run(content::CertificateRequestResultType::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE);
  } else {
    callback->Run(content::CertificateRequestResultType::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
  }

  pending_cert_error_callbacks_.Remove(id);
}

void XWalkContentsClientBridge::RunJavaScriptDialog(content::JavaScriptDialogType dialog_type, const GURL& origin_url,
                                                    const base::string16& message_text,
                                                    const base::string16& default_prompt_text,
                                                    content::JavaScriptDialogManager::DialogClosedCallback callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  JNIEnv* env = AttachCurrentThread();

  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null()) {
    std::move(callback).Run(false, base::string16());
    return;
  }

  int callback_id = pending_js_dialog_callbacks_.Add(
      base::MakeUnique<content::JavaScriptDialogManager::DialogClosedCallback>(std::move(callback)));
  ScopedJavaLocalRef<jstring> jurl(ConvertUTF8ToJavaString(env, origin_url.spec()));
  ScopedJavaLocalRef<jstring> jmessage(ConvertUTF16ToJavaString(env, message_text));

  switch (dialog_type) {
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      Java_XWalkContentsClientBridge_handleJsAlert(env, obj, jurl, jmessage, callback_id);
      break;
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
      Java_XWalkContentsClientBridge_handleJsConfirm(env, obj, jurl, jmessage, callback_id);
      break;
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT: {
      ScopedJavaLocalRef<jstring> jdefault_value(ConvertUTF16ToJavaString(env, default_prompt_text));
      Java_XWalkContentsClientBridge_handleJsPrompt(env, obj, jurl, jmessage, jdefault_value, callback_id);
      break;
    }
    default:
      NOTREACHED();
  }
}

void XWalkContentsClientBridge::RunBeforeUnloadDialog(const GURL& origin_url,
                                                      content::JavaScriptDialogManager::DialogClosedCallback callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  JNIEnv* env = AttachCurrentThread();

  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null()) {
    std::move(callback).Run(false, base::string16());
    return;
  }

  const base::string16 message_text = l10n_util::GetStringUTF16(IDS_BEFOREUNLOAD_MESSAGEBOX_MESSAGE);

  int callback_id = pending_js_dialog_callbacks_.Add(
      base::MakeUnique<content::JavaScriptDialogManager::DialogClosedCallback>(std::move(callback)));
  ScopedJavaLocalRef<jstring> jurl(ConvertUTF8ToJavaString(env, origin_url.spec()));
  ScopedJavaLocalRef<jstring> jmessage(ConvertUTF16ToJavaString(env, message_text));

  Java_XWalkContentsClientBridge_handleJsBeforeUnload(env, obj, jurl, jmessage, callback_id);
}

bool XWalkContentsClientBridge::OnReceivedHttpAuthRequest(
    const JavaRef<jobject>& handler,
    const std::string& host,
    const std::string& realm) {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return false;

  ScopedJavaLocalRef<jstring> jhost = ConvertUTF8ToJavaString(env, host);
  ScopedJavaLocalRef<jstring> jrealm = ConvertUTF8ToJavaString(env, realm);
  Java_XWalkContentsClientBridge_onReceivedHttpAuthRequest(
      env, obj, handler, jhost, jrealm);
  return true;
}

//static void CancelNotification(
//    JavaObjectWeakGlobalRef java_ref, int notification_id) {
//  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
//  JNIEnv* env = AttachCurrentThread();
//  ScopedJavaLocalRef<jobject> obj = java_ref.get(env);
//  if (obj.is_null())
//    return;
//
//  Java_XWalkContentsClientBridge_cancelNotification(
//      env, obj, notification_id);
//}

void XWalkContentsClientBridge::ShowNotification(
    const content::PlatformNotificationData& notification_data,
    const content::NotificationResources& notification_resources) {
  //TODO(iotto): See how to support Web notifications!
  LOG(WARNING) << __func__ << " Notsupported!";
//  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
//  JNIEnv* env = AttachCurrentThread();
//
//  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
//  if (obj.is_null())
//    return;
//
//  ScopedJavaLocalRef<jstring> jtitle(
//    ConvertUTF16ToJavaString(env, notification_data.title));
//  ScopedJavaLocalRef<jstring> jbody(
//    ConvertUTF16ToJavaString(env, notification_data.body));
//  ScopedJavaLocalRef<jstring> jreplace_id(
//    ConvertUTF8ToJavaString(env, notification_data.tag));
//  ScopedJavaLocalRef<jobject> jicon;
//  if (notification_resources.notification_icon.colorType() !=
//      SkColorType::kUnknown_SkColorType) {
//    jicon = gfx::ConvertToJavaBitmap(
//        &notification_resources.notification_icon);
//  }
//
//  int notification_id = g_next_notification_id_++;
//  Java_XWalkContentsClientBridge_showNotification(
//      env, obj, jtitle, jbody,
//      jreplace_id, jicon, notification_id);
//
//  if (cancel_callback)
//    *cancel_callback = base::Bind(
//        &CancelNotification, java_ref_, notification_id);
}

void XWalkContentsClientBridge::OnWebLayoutPageScaleFactorChanged(
    float page_scale_factor) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;
  Java_XWalkContentsClientBridge_onWebLayoutPageScaleFactorChanged(
      env, obj, page_scale_factor);
}

bool XWalkContentsClientBridge::ShouldOverrideUrlLoading(
    const base::string16& url,
    bool has_user_gesture,
    bool is_redirect,
    bool is_main_frame) {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return false;
  ScopedJavaLocalRef<jstring> jurl = ConvertUTF16ToJavaString(env, url);
  return Java_XWalkContentsClientBridge_shouldOverrideUrlLoading(
      env, obj, jurl, has_user_gesture, is_redirect, is_main_frame);
}

/**
 *
 */
bool XWalkContentsClientBridge::RewriteUrlIfNeeded(const std::string& url,
                                 ui::PageTransition transition_type,
                                 bool isMainFrame,
                                 std::string* new_url) {
#if TENTA_LOG_NET_ENABLE == 1
  LOG(INFO) << "XWalkContentsClientBridge::RewriteUrlIfNeeded " << url << " transition_type=" << transition_type;
#endif

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return false;

  /* TODO ********* move to function ***********/
  using namespace base::android;

  ScopedJavaLocalRef<jclass> jcls = base::android::GetClass(
      env, "com/tenta/xwalk/refactor/RewriteUrlValue");

  jfieldID field_url = env->GetFieldID(jcls.obj(), "url", "Ljava/lang/String;");
  jfieldID field_tr_type = env->GetFieldID(jcls.obj(), "transitionType", "I");
  jfieldID field_is_main_frame = env->GetFieldID(jcls.obj(), "isMainFrame", "Z");

  jmethodID ctor = MethodID::Get<MethodID::TYPE_INSTANCE>(env,
                                                          jcls.obj(),
                                                          "<init>", "()V");

  // create java object (!!!release local ref!!!)
  jobject new_obj = env->NewObject(jcls.obj(), ctor);

  ScopedJavaLocalRef<jstring> jurl = base::android::ConvertUTF8ToJavaString(env, url);

  env->SetIntField(new_obj, field_tr_type, transition_type);
  env->SetObjectField(new_obj, field_url, jurl.obj());
  env->SetBooleanField(new_obj, field_is_main_frame, isMainFrame);

  // call java
  bool did_rewrite = Java_XWalkContentsClientBridge_rewriteUrlIfNeeded(
      env, obj, base::android::JavaParamRef<jobject>(env, new_obj));

  jurl.Reset(env, (jstring)env->GetObjectField(new_obj, field_url));

  env->DeleteLocalRef(new_obj);

  if ( did_rewrite == true && new_url != nullptr) {
    base::android::ConvertJavaStringToUTF8(env, jurl.obj(), new_url);
#if TENTA_LOG_NET_ENABLE == 1
    LOG(INFO) << "GOT rewritten from:" << url << " to:" << *new_url;
#endif
    return did_rewrite;
  }

  return false;
}

/**
 *
 */
void XWalkContentsClientBridge::NewDownload(const GURL& url, const std::string& user_agent,
                                            const std::string& content_disposition, const std::string& mime_type,
                                            int64_t content_length) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  ScopedJavaLocalRef<jstring> jstring_url = ConvertUTF8ToJavaString(env, url.spec());
  ScopedJavaLocalRef<jstring> jstring_user_agent = ConvertUTF8ToJavaString(env, user_agent);
  ScopedJavaLocalRef<jstring> jstring_content_disposition = ConvertUTF8ToJavaString(env, content_disposition);
  ScopedJavaLocalRef<jstring> jstring_mime_type = ConvertUTF8ToJavaString(env, mime_type);

  Java_XWalkContentsClientBridge_onDownloadStart(env, obj, jstring_url, jstring_user_agent, jstring_content_disposition,
                                                 jstring_mime_type, content_length);
}

void XWalkContentsClientBridge::NewLoginRequest(const std::string& realm, const std::string& account, const std::string& args) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  ScopedJavaLocalRef<jstring> jrealm = ConvertUTF8ToJavaString(env, realm);
  ScopedJavaLocalRef<jstring> jargs = ConvertUTF8ToJavaString(env, args);

  ScopedJavaLocalRef<jstring> jaccount;
  if (!account.empty())
    jaccount = ConvertUTF8ToJavaString(env, account);

  Java_XWalkContentsClientBridge_onReceivedLoginRequest(env, obj, jrealm, jaccount, jargs);
}

void XWalkContentsClientBridge::ConfirmJsResult(JNIEnv* env,
                                                jobject,
                                                int id,
                                                jstring prompt) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::JavaScriptDialogManager::DialogClosedCallback* callback =
      pending_js_dialog_callbacks_.Lookup(id);
  if (!callback) {
    LOG(WARNING) << "Unexpected JS dialog confirm. " << id;
    return;
  }

  base::string16 prompt_text;
  if (prompt) {
    prompt_text = ConvertJavaStringToUTF16(env, prompt);
  }

  std::move(*callback).Run(true, prompt_text);
  pending_js_dialog_callbacks_.Remove(id);
}

void XWalkContentsClientBridge::CancelJsResult(JNIEnv*, jobject, int id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  content::JavaScriptDialogManager::DialogClosedCallback* callback =
      pending_js_dialog_callbacks_.Lookup(id);
  if (!callback) {
    LOG(WARNING) << "Unexpected JS dialog cancel. " << id;
    return;
  }

  std::move(*callback).Run(false, base::string16());
  pending_js_dialog_callbacks_.Remove(id);
}

void XWalkContentsClientBridge::NotificationDisplayed(JNIEnv*, jobject, jint notification_id) {
  // TODO(iotto) : Implement
//  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
//  content::DesktopNotificationDelegate* notification_delegate = nullptr;
//  auto it = g_notification_map_.find(notification_id);
//  if ( it != g_notification_map_.end() ) {
//    notification_delegate = it->second.get();
//  }
//
//  if (notification_delegate)
//    notification_delegate->NotificationDisplayed();
}

void XWalkContentsClientBridge::NotificationClicked(JNIEnv*, jobject, jint id) {
  // TODO(iotto) : Implement
//  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
//  std::unique_ptr<content::DesktopNotificationDelegate> notification_delegate;
//
//  auto it = g_notification_map_.find(id);
//  if ( it != g_notification_map_.end() ) {
//    notification_delegate = std::move(it->second);
//    g_notification_map_.erase(it);
//  }
//
//  if (notification_delegate.get())
//    notification_delegate->NotificationClick();
}

void XWalkContentsClientBridge::NotificationClosed(JNIEnv*, jobject, jint id, bool by_user) {
  // TODO(iotto) : Implement
//  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
//  std::unique_ptr<content::DesktopNotificationDelegate> notification_delegate;
//
//  auto it = g_notification_map_.find(id);
//  if ( it != g_notification_map_.end() ) {
//    notification_delegate = std::move(it->second);
//    g_notification_map_.erase(it);
//  }
//
//  if (notification_delegate.get())
//    notification_delegate->NotificationClosed();
}

static void JNI_XWalkContentsClientBridge_OnFilesSelected(JNIEnv* env, const JavaParamRef<jclass>& jclazz, int process_id,
                                                int render_id, int mode_flags, const JavaParamRef<jobjectArray>& file_paths,
                                                const JavaParamRef<jobjectArray>& display_names) {
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(process_id, render_id);
  if (!rfh)
    return;

  std::vector<std::string> file_path_str;
  std::vector<std::string> display_name_str;
  // Note file_paths maybe NULL, but this will just yield a zero-length vector.
  base::android::AppendJavaStringArrayToStringVector(env, file_paths, &file_path_str);
  base::android::AppendJavaStringArrayToStringVector(env, display_names, &display_name_str);
  std::vector<content::FileChooserFileInfo> files;
  files.reserve(file_path_str.size());
  for (size_t i = 0; i < file_path_str.size(); ++i) {
    GURL url(file_path_str[i]);
    if (!url.is_valid()) {
      TENTA_LOG_NET(WARNING) << __func__ << " invalid_url=" << file_path_str[i];
      continue;
    }
    base::FilePath path(
        url.SchemeIsFile() ?
            net::UnescapeURLComponent(
                url.path(), net::UnescapeRule::SPACES | net::UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS) :
            file_path_str[i]);
    content::FileChooserFileInfo file_info;
    file_info.file_path = path;
    if (!display_name_str[i].empty())
      file_info.display_name = display_name_str[i];
    TENTA_LOG_NET(INFO) << __func__ << " new_file=" << path;
    files.push_back(file_info);
  }
  FileChooserParams::Mode mode = static_cast<content::FileChooserParams::Mode>(mode_flags);
//  if (mode_flags & kFileChooserModeOpenFolder) {
//    mode = FileChooserParams::UploadFolder;
//  } else if (mode_flags & kFileChooserModeOpenMultiple) {
//    mode = FileChooserParams::OpenMultiple;
//  } else {
//    mode = FileChooserParams::Open;
//  }
  TENTA_LOG_NET(INFO) << __func__ << " mode=" << mode << " files_cnt=" << files.size() << " file paths="
                      << base::JoinString(file_path_str, ":");
  rfh->FilesSelectedInChooser(files, mode);
}

void XWalkContentsClientBridge::DownloadIcon(JNIEnv* env, jobject obj, jstring url) {
  // TODO(iotto) : refactor this, move icon_helper to contents
  std::string url_str = base::android::ConvertJavaStringToUTF8(env, url);
  if (icon_helper_.get() != nullptr) {
    icon_helper_->DownloadIcon(GURL(url_str));
  }
}

void XWalkContentsClientBridge::OnIconAvailable(const GURL& icon_url) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);

  ScopedJavaLocalRef<jstring> jurl(
      ConvertUTF8ToJavaString(env, icon_url.spec()));

  Java_XWalkContentsClientBridge_onIconAvailable(env, obj, jurl);
}

void XWalkContentsClientBridge::OnReceivedIcon(const GURL& icon_url,
                                               const SkBitmap& bitmap) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  content::NavigationEntry* entry =
      icon_helper_->web_contents()->GetController().GetLastCommittedEntry();

  if (entry) {
    entry->GetFavicon().valid = true;
    entry->GetFavicon().url = icon_url;
    entry->GetFavicon().image = gfx::Image::CreateFrom1xBitmap(bitmap);
  }

  ScopedJavaLocalRef<jstring> jurl(
      ConvertUTF8ToJavaString(env, icon_url.spec()));
  ScopedJavaLocalRef<jobject> jicon = gfx::ConvertToJavaBitmap(&bitmap);

  Java_XWalkContentsClientBridge_onReceivedIcon(
      env, obj, jurl, jicon);
}

bool RegisterXWalkContentsClientBridge(JNIEnv* env) {
//  return RegisterNativesImpl(env);
  return false;
}

// This method is inspired by OnSystemRequestCompletion() in
// chrome/browser/ui/android/ssl_client_certificate_request.cc
void XWalkContentsClientBridge::ProvideClientCertificateResponse(
    JNIEnv* env,
    jobject obj,
    int request_id,
    const JavaRef<jobjectArray>& encoded_chain_ref,
    const JavaRef<jobject>& private_key_ref) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  std::unique_ptr<content::ClientCertificateDelegate> delegate =
      pending_client_cert_request_delegates_.Replace(request_id, nullptr);
  pending_client_cert_request_delegates_.Remove(request_id);
  DCHECK(delegate);

  if (encoded_chain_ref.is_null() || private_key_ref.is_null()) {
    LOG(ERROR) << "No client certificate selected";
    delegate->ContinueWithCertificate(nullptr, nullptr);
    return;
  }

  // Convert the encoded chain to a vector of strings.
  std::vector<std::string> encoded_chain_strings;
  if (!encoded_chain_ref.is_null()) {
    base::android::JavaArrayOfByteArrayToStringVector(
        env, encoded_chain_ref.obj(), &encoded_chain_strings);
  }

  std::vector<base::StringPiece> encoded_chain;
  for (size_t i = 0; i < encoded_chain_strings.size(); ++i)
    encoded_chain.push_back(encoded_chain_strings[i]);

  // Create the X509Certificate object from the encoded chain.
  scoped_refptr<net::X509Certificate> client_cert(
      net::X509Certificate::CreateFromDERCertChain(encoded_chain));
  if (!client_cert.get()) {
    LOG(ERROR) << "Could not decode client certificate chain";
    return;
  }

  // Create an SSLPrivateKey wrapper for the private key JNI reference.
  scoped_refptr<net::SSLPrivateKey> private_key =
      net::WrapJavaPrivateKey(client_cert.get(), private_key_ref);
  if (!private_key) {
    LOG(ERROR) << "Could not create OpenSSL wrapper for private key";
    return;
  }

  delegate->ContinueWithCertificate(std::move(client_cert),
                                    std::move(private_key));
}

//// Use to cleanup if there is an error in client certificate response.
//void XWalkContentsClientBridge::HandleErrorInClientCertificateResponse(
//    int request_id) {
//  content::ClientCertificateDelegate* delegate =
//      pending_client_cert_request_delegates_.Lookup(request_id);
//  pending_client_cert_request_delegates_.Remove(request_id);
//
//  delete delegate;
//}

void XWalkContentsClientBridge::SelectClientCertificate(net::SSLCertRequestInfo* cert_request_info,
                                                        std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  JNIEnv* env = base::android::AttachCurrentThread();
  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  // Build the |key_types| JNI parameter, as a String[]
  std::vector<std::string> key_types;
  for (size_t i = 0; i < cert_request_info->cert_key_types.size(); ++i) {
    switch (cert_request_info->cert_key_types[i]) {
      case net::CLIENT_CERT_RSA_SIGN:
        key_types.push_back("RSA");
        break;
      case net::CLIENT_CERT_ECDSA_SIGN:
        key_types.push_back("ECDSA");
        break;
      default:
        // Ignore unknown types.
        break;
    }
  }

  ScopedJavaLocalRef<jobjectArray> key_types_ref = base::android::ToJavaArrayOfStrings(env, key_types);
  if (key_types_ref.is_null()) {
    LOG(ERROR) << "Could not create key types array (String[])";
    return;
  }

  // Build the |encoded_principals| JNI parameter, as a byte[][]
  ScopedJavaLocalRef<jobjectArray> principals_ref = base::android::ToJavaArrayOfByteArray(
      env, cert_request_info->cert_authorities);
  if (principals_ref.is_null()) {
    LOG(ERROR) << "Could not create principals array (byte[][])";
    return;
  }

  // Build the |host_name| and |port| JNI parameters, as a String and
  // a jint.
  ScopedJavaLocalRef<jstring> host_name_ref = base::android::ConvertUTF8ToJavaString(
      env, cert_request_info->host_and_port.host());

  int request_id = pending_client_cert_request_delegates_.Add(std::move(delegate));
  Java_XWalkContentsClientBridge_selectClientCertificate(env, obj, request_id, key_types_ref, principals_ref,
                                                         host_name_ref, cert_request_info->host_and_port.port());
}

void XWalkContentsClientBridge::ClearClientCertPreferences(
    JNIEnv* env, jobject obj,
    const JavaParamRef<jobject>& callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  ScopedJavaGlobalRef<jobject>* j_callback = new ScopedJavaGlobalRef<jobject>();
  j_callback->Reset(env, callback);
  content::BrowserThread::PostTaskAndReply(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(&NotifyClientCertificatesChanged),
      base::Bind(&XWalkContentsClientBridge::ClientCertificatesCleared,
          base::Unretained(this), base::Owned(j_callback)));
}

void XWalkContentsClientBridge::ClientCertificatesCleared(
    ScopedJavaGlobalRef<jobject>* callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  JNIEnv* env = AttachCurrentThread();

  ScopedJavaLocalRef<jobject> obj = java_ref_.get(env);
  if (obj.is_null())
    return;

  Java_XWalkContentsClientBridge_clientCertificatesCleared(
      env, obj, *callback);
}

}  // namespace xwalk
