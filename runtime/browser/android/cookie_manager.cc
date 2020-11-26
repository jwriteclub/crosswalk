// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/browser/android/cookie_manager.h"

#include <string>

#include "base/android/jni_string.h"
#include "base/android/jni_array.h"
#include "base/android/path_utils.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/message_loop/message_loop.h"
#include "base/lazy_instance.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/pickle.h"
#include "base/single_thread_task_runner.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/sequenced_worker_pool.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/cookie_store_factory.h"
#include "content/public/common/url_constants.h"
#include "net/cookies/canonical_cookie.h"

#include "jni/XWalkCookieManager_jni.h"
#include "net/cookies/cookie_monster.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_store.h"
#include "net/cookies/cookie_util.h"
#include "net/url_request/url_request_context.h"
#include "ui/base/resource/resource_bundle.h"
#include "xwalk/runtime/browser/android/scoped_allow_wait_for_legacy_web_view_api.h"
#include "xwalk/runtime/browser/android/xwalk_cookie_access_policy.h"
#include "xwalk/runtime/browser/xwalk_browser_main_parts_android.h"
#include "xwalk/runtime/common/xwalk_switches.h"

#include "meta_logging.h"
#if defined(TENTA_CHROMIUM_BUILD)
#include "tenta_cookie_store.h"
#endif

using base::FilePath;
using base::android::ConvertJavaStringToUTF8;
using base::android::ConvertJavaStringToUTF16;
using base::android::JavaParamRef;
using base::android::ScopedJavaLocalRef;
using content::BrowserThread;
using net::CookieList;

// All functions on the CookieManager can be called from any thread, including
// threads without a message loop. BrowserThread::FILE is used to call methods
// on CookieMonster that needs to be called, and called back, on a chrome
// thread.

namespace xwalk {

namespace {

// Are cookies allowed for file:// URLs by default?
const bool kDefaultFileSchemeAllowed = false;
const char kPreKitkatDataDirectory[] = "app_database";
const char kKitkatDataDirectory[] = "app_webview";

void ImportKitkatDataIfNecessary(const base::FilePath& old_data_dir,
                                 const base::FilePath& profile) {
  if (!base::DirectoryExists(old_data_dir))
    return;

  const char* possible_data_dir_names[] = { "Cache", "Cookies",
      "Cookies-journal", "IndexedDB", "Local Storage", };
  for (size_t i = 0; i < arraysize(possible_data_dir_names); i++) {
    base::FilePath dir = old_data_dir.Append(possible_data_dir_names[i]);
    if (base::PathExists(dir)) {
      if (!base::Move(dir, profile.Append(possible_data_dir_names[i]))) {
        NOTREACHED() << "Failed to import previous user data: "
                        << possible_data_dir_names[i];
      }
    }
  }
}

void ImportPreKitkatDataIfNecessary(const base::FilePath& old_data_dir,
                                    const base::FilePath& profile) {
  if (!base::DirectoryExists(old_data_dir))
    return;

  // Local Storage.
  base::FilePath local_storage_path = old_data_dir.Append("localstorage");
  if (base::PathExists(local_storage_path)) {
    if (!base::Move(local_storage_path, profile.Append("Local Storage"))) {
      NOTREACHED() << "Failed to import previous user data: localstorage";
    }
  }
}

void MoveUserDataDirIfNecessary(const base::FilePath& user_data_dir,
                                const base::FilePath& profile) {
  if (base::DirectoryExists(profile))
    return;

  if (!base::CreateDirectory(profile))
    return;

  // Import pre-crosswalk-8 data.
  ImportKitkatDataIfNecessary(user_data_dir, profile);
  // Import Android Kitkat System webview data.
  base::FilePath old_data_dir = user_data_dir.DirName().Append(
      kKitkatDataDirectory);
  ImportKitkatDataIfNecessary(old_data_dir, profile);
  // Import pre-Kitkat System webview data.
  old_data_dir = user_data_dir.DirName().Append(kPreKitkatDataDirectory);
  ImportPreKitkatDataIfNecessary(old_data_dir, profile);
}

void GetUserDataDir(FilePath* user_data_dir) {
  if (!PathService::Get(base::DIR_ANDROID_APP_DATA, user_data_dir)) {
    NOTREACHED() << "Failed to get app data directory for Android WebView";
  }
}

class CookieByteArray {
 public:
  CookieByteArray(const char* buffer, int len) {
    _data = new char[len];
    if (_data != nullptr) {
      memcpy(_data, buffer, len);
      _len = len;
    }
  }

  ~CookieByteArray() {
    delete[] _data;
#if TENTA_LOG_COOKIE_ENABLE == 1
    LOG(INFO) << "Delete Cookie byte array " << _len;
#endif
  }

  const char * data() {
    return _data;
  }
  int len() {
    return _len;
  }

 private:
  char * _data;
  int _len;
};

// CookieManager creates and owns XWalkView's CookieStore, in addition to
// handling calls into the CookieStore from Java.
//
// Since Java calls can be made on the IO Thread, and must synchronously return
// a result, and the CookieStore API allows it to asynchronously return results,
// the CookieStore must be run on its own thread, to prevent deadlock.
class CookieManager {
 public:
  static CookieManager* GetInstance();

// Returns the TaskRunner on which the CookieStore lives.
  base::SingleThreadTaskRunner* GetCookieStoreTaskRunner();
// Returns the CookieStore, creating it if necessary. This must only be called
// on the CookieStore TaskRunner.
  net::CookieStore* GetCookieStore();

  void SaveCookies(base::Pickle *dstPickle);
  void SaveCookiesAsyncHelper(base::Pickle *dstPickle,
                              base::WaitableEvent* completion);

  void SaveCookiesCompleted(base::Pickle *dstPickle,
                            base::WaitableEvent* completion,
                            const net::CookieList& cookies);

  void RestoreCookies(CookieByteArray * cb);
  void RestoreCookiesAsyncHelper(CookieByteArray * cb,
                                 base::WaitableEvent* completion);
  void RestoreCookieCallback(bool success);

  void SetAcceptCookie(bool accept);
  bool AcceptCookie();
  void SetCookie(const GURL& host, const std::string& cookie_value);
  std::string GetCookie(const GURL& host);
  void RemoveSessionCookie();
  void RemoveAllCookie();
  void ClearZoneCookies(const std::string& zone);
  void RemoveExpiredCookie();
  void FlushCookieStore();
  bool HasCookies();
  bool AllowFileSchemeCookies();
  void SetAcceptFileSchemeCookies(bool accept);

  void SetDbKey(const std::string& dbKey);
  int RekeyDb(const std::string& oldKey, const std::string& newKey);
  void SetZone(const std::string& zone);
  int NukeDomain(const std::string& domain);
  void PageLoadStarted(const std::string& loadingUrl);

  void Reset();

 private:
  friend struct base::LazyInstanceTraitsBase<CookieManager>;

  CookieManager();
  ~CookieManager();

  typedef base::Callback<void(base::WaitableEvent*)> CookieTask;
  void ExecCookieTask(const CookieTask& task, const bool wait_for_completion);

  void SetCookieAsyncHelper(const GURL& host, const std::string& value,
                            base::WaitableEvent* completion);
  void SetCookieCompleted(bool success);

  void GetCookieValueAsyncHelper(const GURL& host, std::string* result,
                                 base::WaitableEvent* completion);
  void GetCookieValueCompleted(base::WaitableEvent* completion,
                               std::string* result, const std::string& value);

  void RemoveSessionCookieAsyncHelper(base::WaitableEvent* completion);
  void RemoveAllCookieAsyncHelper(base::WaitableEvent* completion);
  void RemoveCookiesCompleted(base::WaitableEvent* completion, uint32_t num_deleted);

  void ClearZoneCookiesTask(const std::string& zone, base::WaitableEvent* completion);
//  void ClearZoneCookiesDone(const std::string& zone, base::WaitableEvent* completion);

  void FlushCookieStoreAsyncHelper(base::WaitableEvent* completion);

  void HasCookiesAsyncHelper(bool* result, base::WaitableEvent* completion);
  void HasCookiesCompleted(base::WaitableEvent* completion, bool* result,
                           const net::CookieList& cookies);

#ifdef TENTA_CHROMIUM_BUILD
  void TriggerCookieFetchAsyncHelper(base::WaitableEvent* completion);
  void NukeDomainAsyncHelper(const std::string& domain, base::WaitableEvent* completion);
  void SetDbKeyAsyncHelper(const std::string& dbKey, base::WaitableEvent* completion);
  void RekeyDbAsyncHelper(const std::string& oldKey, const std::string& newKey, int* resultHolder, base::WaitableEvent* completion);
  void SetZoneAsyncHelper(const std::string& zone, base::WaitableEvent* completion);
  void SetZoneDoneDelete(const std::string& zone, uint32_t num_deleted);
  void PageLoadStartedAsyncHelper(const std::string& loadingUrl, base::WaitableEvent* completion);
#endif

// This protects the following two bools, as they're used on multiple threads.
  base::Lock accept_file_scheme_cookies_lock_;
// True if cookies should be allowed for file URLs. Can only be changed prior
// to creating the CookieStore.
  bool accept_file_scheme_cookies_;
// True once the cookie store has been created. Just used to track when
// |accept_file_scheme_cookies_| can no longer be modified.
  bool cookie_store_created_;

  base::Thread cookie_store_client_thread_;
  base::Thread cookie_store_backend_thread_;

  scoped_refptr<base::SingleThreadTaskRunner> cookie_store_task_runner_;
  std::unique_ptr<net::CookieStore> cookie_store_;

#ifdef TENTA_CHROMIUM_BUILD
  scoped_refptr<tenta::ext::TentaCookieStore> _tenta_store;
#endif

  DISALLOW_COPY_AND_ASSIGN(CookieManager)
  ;
};

base::LazyInstance<CookieManager>::Leaky g_lazy_instance;

}  // namespace

// static
CookieManager* CookieManager::GetInstance() {
  return g_lazy_instance.Pointer();
}

CookieManager::CookieManager()
    : accept_file_scheme_cookies_(kDefaultFileSchemeAllowed),
      cookie_store_created_(false),
      cookie_store_client_thread_("CookieMonsterClient"),
      cookie_store_backend_thread_("CookieMonsterBackend") {
  // make MessageLoopForIO type!
  base::Thread::Options op(base::MessageLoop::Type::TYPE_IO, 0 /*default stack size*/);
  cookie_store_client_thread_.StartWithOptions(op);
  cookie_store_task_runner_ = cookie_store_client_thread_.task_runner();
//  cookie_store_task_runner_ = BrowserThread::GetTaskRunnerForThread(BrowserThread::IO);

  cookie_store_backend_thread_.Start();
  TENTA_LOG_COOKIE(INFO) << __func__ << " thread_id=" << cookie_store_client_thread_.GetThreadId();
}

CookieManager::~CookieManager() {
}

// Executes the |task| on the FILE thread. |wait_for_completion| should only be
// true if the Java API method returns a value or is explicitly stated to be
// synchronous.
void CookieManager::ExecCookieTask(const CookieTask& task, const bool wait_for_completion) {
  base::WaitableEvent completion(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                                 base::WaitableEvent::InitialState::NOT_SIGNALED);
  bool executed = cookie_store_task_runner_->PostTask(FROM_HERE,
  base::Bind(task, wait_for_completion ? &completion : nullptr));

  TENTA_LOG_COOKIE(INFO) << __func__ << " executed=" << executed;

  if (executed && wait_for_completion) {
    ScopedAllowWaitForLegacyWebViewApi wait;
    completion.Wait();
  }
}

base::SingleThreadTaskRunner* CookieManager::GetCookieStoreTaskRunner() {
  return cookie_store_task_runner_.get();
}

net::CookieStore* CookieManager::GetCookieStore() {
  DCHECK(cookie_store_task_runner_->BelongsToCurrentThread());

  if (!cookie_store_) {
    FilePath user_data_dir;
    GetUserDataDir(&user_data_dir);
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    if (command_line->HasSwitch(switches::kXWalkProfileName)) {
      base::FilePath profile = user_data_dir.Append(command_line->GetSwitchValuePath(switches::kXWalkProfileName));
      MoveUserDataDirIfNecessary(user_data_dir, profile);
      user_data_dir = profile;
    }

    FilePath cookie_store_path = user_data_dir.Append(FILE_PATH_LITERAL("Cookies"));

#if TENTA_LOG_COOKIE_ENABLE == 1
    LOG(INFO) << "!!! " << __func__ << " cookie_store_path=" << cookie_store_path.value();
#endif
    content::CookieStoreConfig cookie_config(cookie_store_path, content::CookieStoreConfig::RESTORED_SESSION_COOKIES,
                                             nullptr);
    // TODO(iotto) : If not set will be posted to BrowerThread::IO
    cookie_config.client_task_runner = cookie_store_task_runner_;

    // TODO(iotto): Start with option block shutdown
    cookie_config.background_task_runner = cookie_store_backend_thread_.task_runner();

    {
      base::AutoLock lock(accept_file_scheme_cookies_lock_);

      // There are some unknowns about how to correctly handle file:// cookies,
      // and our implementation for this is not robust.  http://crbug.com/582985
      //
      // TODO(mmenke): This call should be removed once we can deprecate and
      // remove the Android WebView 'CookieManager::setAcceptFileSchemeCookies'
      // method. Until then, note that this is just not a great idea.
      cookie_config.cookieable_schemes.insert(
          cookie_config.cookieable_schemes.begin(), net::CookieMonster::kDefaultCookieableSchemes,
          net::CookieMonster::kDefaultCookieableSchemes + net::CookieMonster::kDefaultCookieableSchemesCount);
      if (accept_file_scheme_cookies_)
        cookie_config.cookieable_schemes.push_back(url::kFileScheme);
      cookie_store_created_ = true;
    }
#if defined(TENTA_CHROMIUM_BUILD)
    cookie_store_ = tenta::ext::CreateCookieStore(cookie_config, _tenta_store);
#else
    cookie_store_ = content::CreateCookieStore(cookie_config);
#endif
  }

  return cookie_store_.get();
}

void CookieManager::SetAcceptCookie(bool accept) {
  XWalkCookieAccessPolicy::GetInstance()->SetGlobalAllowAccess(accept);
}

bool CookieManager::AcceptCookie() {
  return XWalkCookieAccessPolicy::GetInstance()->GetGlobalAllowAccess();
}

void CookieManager::SetCookie(const GURL& host, const std::string& cookie_value) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " host=" << host << " cookie=" << cookie_value;

  ExecCookieTask(base::Bind(&CookieManager::SetCookieAsyncHelper, base::Unretained(this), host, cookie_value),
  false);
}

void CookieManager::SetCookieAsyncHelper(const GURL& host, const std::string& value, base::WaitableEvent* completion) {
  DCHECK(!completion);
  net::CookieOptions options;
  options.set_include_httponly();

  GetCookieStore()->SetCookieWithOptionsAsync(host, value, options,
                                              base::Bind(&CookieManager::SetCookieCompleted, base::Unretained(this)));
}

void CookieManager::SetCookieCompleted(bool success) {
// The CookieManager API does not return a value for SetCookie,
// so we don't need to propagate the |success| value back to the caller.
  TENTA_LOG_COOKIE(INFO) << __func__ << " success=" << success;
}

std::string CookieManager::GetCookie(const GURL& host) {
  std::string cookie_value;
  ExecCookieTask(base::Bind(&CookieManager::GetCookieValueAsyncHelper, base::Unretained(this), host, &cookie_value),
  true);

  return cookie_value;
}

void CookieManager::GetCookieValueAsyncHelper(const GURL& host, std::string* result, base::WaitableEvent* completion) {
  net::CookieOptions options;
  options.set_include_httponly();

  GetCookieStore()->GetCookiesWithOptionsAsync(
      host, options, base::Bind(&CookieManager::GetCookieValueCompleted, base::Unretained(this), completion, result));
}

void CookieManager::GetCookieValueCompleted(base::WaitableEvent* completion, std::string* result,
                                            const std::string& value) {
  *result = value;
  DCHECK(completion);
  completion->Signal();
}

void CookieManager::RemoveSessionCookie() {
  ExecCookieTask(base::Bind(&CookieManager::RemoveSessionCookieAsyncHelper, base::Unretained(this)),
  false);
}

void CookieManager::RemoveSessionCookieAsyncHelper(base::WaitableEvent* completion) {
  DCHECK(!completion);
  GetCookieStore()->DeleteSessionCookiesAsync(
      base::BindOnce(&CookieManager::RemoveCookiesCompleted, base::Unretained(this), completion));
}

void CookieManager::RemoveAllCookie() {
  TENTA_LOG_COOKIE(INFO) << __func__;
  ExecCookieTask(base::Bind(&CookieManager::RemoveAllCookieAsyncHelper, base::Unretained(this)),
  false);
}

// TODO(kristianm): Pass a null callback so it will not be invoked
// across threads.
void CookieManager::RemoveAllCookieAsyncHelper(base::WaitableEvent* completion) {
  DCHECK(!completion);
  TENTA_LOG_COOKIE(INFO) << __func__;

  // calls CookieMonster
  GetCookieStore()->DeleteAllAsync(
      base::BindOnce(&CookieManager::RemoveCookiesCompleted, base::Unretained(this), completion));
}

void CookieManager::RemoveCookiesCompleted(base::WaitableEvent* completion, uint32_t num_deleted) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " deleted=" << num_deleted;
// The CookieManager API does not return a value for removeSessionCookie or
// removeAllCookie, so we don't need to propagate the |num_deleted| value
// back to the caller.
  if ( completion != nullptr ) {
    completion->Signal();
  }
}

void CookieManager::ClearZoneCookies(const std::string& zone) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " zone=" << zone;
  ExecCookieTask(base::Bind(&CookieManager::ClearZoneCookiesTask, base::Unretained(this), zone),
  true);
}

void CookieManager::ClearZoneCookiesTask(const std::string& zone, base::WaitableEvent* completion) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " zone=" << zone;

  GetCookieStore();
  // if zone == current_zone invoke deleteAllAsync
  // else delete folder from db
  if (_tenta_store->zone().compare(zone) == 0 ) { // clear for same zone as current
    RemoveAllCookieAsyncHelper(completion);
  } else {

    // TODO(iotto): Invoke with callback and use ClearZoneCookiesDone
    _tenta_store->DeleteZoneCookies(zone);
    if ( completion != nullptr ) {
      completion->Signal();
    }
  }
}

//void CookieManager::ClearZoneCookiesDone(const std::string& zone, base::WaitableEvent* completion) {
//  TENTA_LOG_COOKIE(INFO) << __func__ << " zone=" << zone;
//
//  if ( completion != nullptr ) {
//    completion->Signal();
//  }
//}

void CookieManager::RemoveExpiredCookie() {
// HasCookies will call GetAllCookiesAsync, which in turn will force a GC.
  HasCookies();
}

void CookieManager::FlushCookieStoreAsyncHelper(base::WaitableEvent* completion) {
  DCHECK(!completion);
  GetCookieStore()->FlushStore(base::Bind(&base::DoNothing));
}

void CookieManager::FlushCookieStore() {
  ExecCookieTask(base::Bind(&CookieManager::FlushCookieStoreAsyncHelper, base::Unretained(this)),
  false);
}

/**
 *
 */
static ScopedJavaLocalRef<jbyteArray> JNI_XWalkCookieManager_SaveCookies(JNIEnv* env,
                                                                                 const JavaParamRef<jobject>& jcaller) {

  base::Pickle pickle;

  CookieManager::GetInstance()->SaveCookies(&pickle);

  return base::android::ToJavaByteArray(env, reinterpret_cast<const uint8_t*>(pickle.data()), pickle.size());
}

/**
 *
 */
static jboolean JNI_XWalkCookieManager_RestoreCookies(JNIEnv* env, const JavaParamRef<jobject>& jcaller,
                                                              const JavaParamRef<jbyteArray>& data) {

  jbyte* _bytes = nullptr;

  if (data.obj() != nullptr) {
    jsize len = env->GetArrayLength(data.obj());  // array length

    if (len > 0) {
      _bytes = env->GetByteArrayElements(data.obj(), nullptr);

      CookieByteArray *cb = new CookieByteArray(reinterpret_cast<const char*>(_bytes), len);

      CookieManager::GetInstance()->RestoreCookies(cb);

      env->ReleaseByteArrayElements(data.obj(), _bytes, JNI_ABORT);

      return true;
    }  // len zero
  }  // data null

  CookieManager::GetInstance()->RemoveAllCookie();
  return false;
}

/**
 * Save cookies in pickle
 */
void CookieManager::SaveCookies(base::Pickle *dstPickle) {
#if TENTA_LOG_COOKIE_ENABLE == 1
  LOG(INFO) << "SaveCookies";
#endif
  FlushCookieStore();

  ExecCookieTask(base::Bind(&CookieManager::SaveCookiesAsyncHelper, base::Unretained(this), dstPickle),
  true);
#if TENTA_LOG_COOKIE_ENABLE == 1
  LOG(INFO) << "SaveCookies return";
#endif
}

void CookieManager::SaveCookiesAsyncHelper(base::Pickle *dstPickle, base::WaitableEvent* completion) {
  GetCookieStore()->GetAllCookiesAsync(
      base::Bind(&CookieManager::SaveCookiesCompleted, base::Unretained(this), dstPickle, completion));
}

void CookieManager::SaveCookiesCompleted(base::Pickle *pickle, base::WaitableEvent* completion,
                                         const net::CookieList& cookies) {
  DCHECK(pickle != nullptr);

// write list to pickle
  pickle->WriteInt(cookies.size());

  for (net::CookieList::const_iterator it = cookies.begin(); it != cookies.end(); ++it) {
    const net::CanonicalCookie& cc = *it;

#if TENTA_LOG_COOKIE_ENABLE == 1
    LOG(INFO) << "Cookie saved name=" << cc.Name() << " value=" << cc.Value() << " domain=" << cc.Domain();
#endif
//    pickle->WriteString(cc.Source().spec());
    pickle->WriteString(cc.Name());
    pickle->WriteString(cc.Value());
    pickle->WriteString(cc.Domain());
    pickle->WriteString(cc.Path());
    pickle->WriteInt64(cc.CreationDate().ToInternalValue());
    pickle->WriteInt64(cc.ExpiryDate().ToInternalValue());
    pickle->WriteInt64(cc.LastAccessDate().ToInternalValue());
    pickle->WriteBool(cc.IsSecure());
    pickle->WriteBool(cc.IsHttpOnly());
    pickle->WriteInt(static_cast<int>(cc.SameSite()));
    pickle->WriteInt(cc.Priority());
  }

// the end
  completion->Signal();
}

/**
 * Save cookies in pickle
 */
void CookieManager::RestoreCookies(CookieByteArray * cb) {
#if TENTA_LOG_COOKIE_ENABLE == 1
  int len;

  if (cb != nullptr) {
    len = cb->len();
  }

  LOG(INFO) << "RestoreCookies " << len;
#endif
  RemoveAllCookie();

  ExecCookieTask(base::Bind(&CookieManager::RestoreCookiesAsyncHelper, base::Unretained(this), base::Owned(cb)),
  false);

#if TENTA_LOG_COOKIE_ENABLE == 1
  LOG(INFO) << "RestoreCookies return ";
#endif
}

void CookieManager::RestoreCookiesAsyncHelper(CookieByteArray * cb, base::WaitableEvent* completion) {

  if (cb != nullptr || cb->data() != nullptr) {
    net::CookieStore* cs = GetCookieStore();

    base::Pickle pickle(cb->data(), cb->len());
    base::PickleIterator it(pickle);

    net::CookieStore::SetCookiesCallback callback = base::Bind(&CookieManager::RestoreCookieCallback,
                                                               base::Unretained(this));

    int cnt;  // cookie count

    if (it.ReadInt(&cnt) && cnt > 0) {

      for (int i = 0; i < cnt; ++i) {
        std::string name, value, domain, path/*, spec*/;
        int64_t c_time, e_time, l_time;
        bool secure, http_only;
        int same_site, prio;
        // url     std::string key(cookie_util::GetEffectiveDomain(url.scheme(), url.host()));

        if (/*it.ReadString(&spec) && */it.ReadString(&name) && it.ReadString(&value) && it.ReadString(&domain)
            && it.ReadString(&path) && it.ReadInt64(&c_time) && it.ReadInt64(&e_time) && it.ReadInt64(&l_time)
            && it.ReadBool(&secure) && it.ReadBool(&http_only) && it.ReadInt(&same_site) && it.ReadInt(&prio)) {

          GURL url = net::cookie_util::CookieOriginToURL(domain, secure);

          std::unique_ptr < net::CanonicalCookie > cookie = net::CanonicalCookie::CreateSanitizedCookie(
              url, name, value, domain, path, base::Time::FromInternalValue(c_time),
              base::Time::FromInternalValue(e_time), base::Time::FromInternalValue(l_time), secure, http_only,
              static_cast<net::CookieSameSite>(same_site), static_cast<net::CookiePriority>(prio));

          cs->SetCanonicalCookieAsync(std::move(cookie), true /*secure_source*/, true /*modify_http_only*/,
                                      std::move(callback));

#if TENTA_LOG_COOKIE_ENABLE == 1
          LOG(INFO) << "Cookie restored name=" << name << " value=" << value << " domain=" << domain << " url="
                    << url.spec();
#endif

        } else {
          LOG(WARNING) << "Pickle error: |" << name << "|" << domain;
          break;  // if NOT all read
        }
      }  // for
    } else {
      LOG(WARNING) << "Error cookie count " << cnt;
    }
  } else {  // cb data null
    LOG(WARNING) << "Restore cookie not enough memory!";
  }

  //
  if (completion != nullptr) {
    completion->Signal();
  }
}

void CookieManager::RestoreCookieCallback(bool success) {
  TENTA_LOG_COOKIE(INFO) << "!!! " << __func__ << " success=" << (success == true ? "true" : "false");
}

bool CookieManager::HasCookies() {
  bool has_cookies;
  ExecCookieTask(base::Bind(&CookieManager::HasCookiesAsyncHelper, base::Unretained(this), &has_cookies),
  true);
  return has_cookies;
}

// TODO(kristianm): Simplify this, copying the entire list around
// should not be needed.
void CookieManager::HasCookiesAsyncHelper(bool* result, base::WaitableEvent* completion) {
  GetCookieStore()->GetAllCookiesAsync(
      base::Bind(&CookieManager::HasCookiesCompleted, base::Unretained(this), completion, result));
}

void CookieManager::HasCookiesCompleted(base::WaitableEvent* completion,
bool* result,
                                        const CookieList& cookies) {
  *result = cookies.size() != 0;
  DCHECK(completion);
  completion->Signal();
}

bool CookieManager::AllowFileSchemeCookies() {
  base::AutoLock lock(accept_file_scheme_cookies_lock_);
  return accept_file_scheme_cookies_;
}

void CookieManager::SetAcceptFileSchemeCookies(bool accept) {
  base::AutoLock lock(accept_file_scheme_cookies_lock_);
// Can only modify this before the cookie store is created.
  if (!cookie_store_created_) {
    accept_file_scheme_cookies_ = accept;
  }
}

void CookieManager::SetDbKey(const std::string& dbKey) {
#ifdef TENTA_CHROMIUM_BUILD
  ExecCookieTask(base::Bind(&CookieManager::SetDbKeyAsyncHelper, base::Unretained(this), dbKey), false);
#endif
}

#ifdef TENTA_CHROMIUM_BUILD
void CookieManager::SetDbKeyAsyncHelper(const std::string& dbKey, base::WaitableEvent* completion) {
  GetCookieStore();
  _tenta_store->SetDbKey(dbKey);
}
#endif // TENTA_CHROMIUM_BUILD

int CookieManager::RekeyDb(const std::string& oldKey, const std::string& newKey) {
#ifdef TENTA_CHROMIUM_BUILD
  int result = 0;

  std::string cookie_value;
  ExecCookieTask(base::Bind(&CookieManager::RekeyDbAsyncHelper, base::Unretained(this), oldKey, newKey, &result), true);

  return result;

#else
  return -1;
#endif
}

#ifdef TENTA_CHROMIUM_BUILD
void CookieManager::RekeyDbAsyncHelper(const std::string& oldKey, const std::string& newKey, int* resultHolder,
                                       base::WaitableEvent* completion) {

  GetCookieStore();
  *resultHolder = _tenta_store->RekeyDb(oldKey, newKey);
  if (completion != nullptr) {
    completion->Signal();
  }
}
#endif

void CookieManager::SetZone(const std::string& zone) {
#ifdef TENTA_CHROMIUM_BUILD
  TENTA_LOG_COOKIE(INFO) << __func__ << " zone=" << zone;
  ExecCookieTask(base::Bind(&CookieManager::SetZoneAsyncHelper, base::Unretained(this), zone),
      false /*wait 'till finish*/);
#endif
}

#ifdef TENTA_CHROMIUM_BUILD
void CookieManager::SetZoneAsyncHelper(const std::string& zone, base::WaitableEvent* completion) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " zone=" << zone;

  GetCookieStore();
  if (_tenta_store->zone().empty()) {  // nothing is loaded so safe to just set the zone
    _tenta_store->ZoneSwitching(true);  // zone switch started
    _tenta_store->ZoneChanged(zone);
    _tenta_store->ZoneSwitching(false);  // done switching zone
  } else if (_tenta_store->zone().compare(zone) != 0) {
    _tenta_store->ZoneSwitching(true);  // zone switch started
    _tenta_store->ZoneChanged(zone);

    GetCookieStore()->DeleteAllAsync(
        base::BindOnce(&CookieManager::SetZoneDoneDelete, base::Unretained(this), zone));
//    RemoveAllCookieAsyncHelper(nullptr);
//    ExecCookieTask(base::Bind(&CookieManager::RemoveAllCookieAsyncHelper, base::Unretained(this)),
//                   true /*wait 'till finish*/);
    // TODO(iotto): do we need to postTask, since we're setting just a flag
    // then we might need synchronization
//    GetCookieStore()->TriggerCookieFetch();
//    TriggerCookieFetchAsyncHelper(nullptr);
//    ExecCookieTask(base::Bind(&CookieManager::TriggerCookieFetchAsyncHelper, base::Unretained(this)), false);
//    _tenta_store->ZoneSwitching(false);  // done switching zone

  }
}

void CookieManager::SetZoneDoneDelete(const std::string& zone, uint32_t num_deleted) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " zone=" << zone << " num_deleted=" << num_deleted;

  GetCookieStore()->TriggerCookieFetch();
  _tenta_store->ZoneSwitching(false);  // done switching zone
}

/**
 *
 */
void CookieManager::TriggerCookieFetchAsyncHelper(base::WaitableEvent* completion) {
  TENTA_LOG_COOKIE(INFO) << __func__;
  GetCookieStore()->TriggerCookieFetch();
}
#endif // TENTA_CHROMIUM_BUILD

int CookieManager::NukeDomain(const std::string& domain) {
#ifdef TENTA_CHROMIUM_BUILD
  ExecCookieTask(base::Bind(&CookieManager::NukeDomainAsyncHelper, base::Unretained(this), domain),
      true /*wait 'till finish*/);
  return 0;
#else
  return 0;
#endif
}

#ifdef TENTA_CHROMIUM_BUILD
void CookieManager::NukeDomainAsyncHelper(const std::string& domain, base::WaitableEvent* completion) {
  net::CookieStore* c_store = GetCookieStore();
  _tenta_store->NukeDomain(domain, c_store);
  if ( completion != nullptr ) {
    completion->Signal();
  }
}
#endif

void CookieManager::PageLoadStarted(const std::string& loadingUrl) {
#ifdef TENTA_CHROMIUM_BUILD
  TENTA_LOG_COOKIE(INFO) << __func__ << " url=" << loadingUrl;
  ExecCookieTask(base::Bind(&CookieManager::PageLoadStartedAsyncHelper, base::Unretained(this), loadingUrl),
  false /*wait 'till finish*/);

#endif
}

#ifdef TENTA_CHROMIUM_BUILD
void CookieManager::PageLoadStartedAsyncHelper(const std::string& loadingUrl, base::WaitableEvent* completion) {
  TENTA_LOG_COOKIE(INFO) << __func__ << " url=" << loadingUrl;

  GetCookieStore();
  _tenta_store->PageLoadStarted(loadingUrl);
}
#endif // TENTA_CHROMIUM_BUILD

void CookieManager::Reset() {
  // TODO(iotto) : Move this too to cookie thread
  LOG(WARNING) << __func__ << " move_to_cookie_thread";
#ifdef TENTA_CHROMIUM_BUILD
  GetCookieStore();
  _tenta_store->ZoneSwitching(true);
  ExecCookieTask(base::Bind(&CookieManager::RemoveAllCookieAsyncHelper, base::Unretained(this)),
  true /*wait 'till finish*/);
  _tenta_store->Reset();
#endif
}

static void JNI_XWalkCookieManager_SetAcceptCookie(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                           jboolean accept) {
  CookieManager::GetInstance()->SetAcceptCookie(accept);
}

static jboolean JNI_XWalkCookieManager_AcceptCookie(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  return CookieManager::GetInstance()->AcceptCookie();
}

static void JNI_XWalkCookieManager_SetCookie(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                     const JavaParamRef<jstring>& url,
                                                     const JavaParamRef<jstring>& value) {
  GURL host(ConvertJavaStringToUTF16(env, url));
  std::string cookie_value(ConvertJavaStringToUTF8(env, value));

  CookieManager::GetInstance()->SetCookie(host, cookie_value);
}

static ScopedJavaLocalRef<jstring> JNI_XWalkCookieManager_GetCookie(JNIEnv* env,
                                                                            const JavaParamRef<jobject>& obj,
                                                                            const JavaParamRef<jstring>& url) {
  GURL host(ConvertJavaStringToUTF16(env, url));

  return base::android::ConvertUTF8ToJavaString(env, CookieManager::GetInstance()->GetCookie(host));
}

static void JNI_XWalkCookieManager_RemoveSessionCookie(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  CookieManager::GetInstance()->RemoveSessionCookie();
}

static void JNI_XWalkCookieManager_RemoveAllCookie(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  CookieManager::GetInstance()->RemoveAllCookie();
}

static void JNI_XWalkCookieManager_ClearZoneCookies(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                           const JavaParamRef<jstring>& zone) {
  std::string zone_str(ConvertJavaStringToUTF8(env, zone));
  CookieManager::GetInstance()->ClearZoneCookies(zone_str);
}

static void JNI_XWalkCookieManager_RemoveExpiredCookie(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  CookieManager::GetInstance()->RemoveExpiredCookie();
}

static void JNI_XWalkCookieManager_FlushCookieStore(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  CookieManager::GetInstance()->FlushCookieStore();
}

static jboolean JNI_XWalkCookieManager_HasCookies(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  return CookieManager::GetInstance()->HasCookies();
}

static jboolean JNI_XWalkCookieManager_AllowFileSchemeCookies(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  return CookieManager::GetInstance()->AllowFileSchemeCookies();
}

static void JNI_XWalkCookieManager_SetAcceptFileSchemeCookies(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                                      jboolean accept) {
  return CookieManager::GetInstance()->SetAcceptFileSchemeCookies(accept);
}

static void JNI_XWalkCookieManager_SetDbKey(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                    const JavaParamRef<jstring>& dbKey) {
  std::string db_key(ConvertJavaStringToUTF8(env, dbKey));

  CookieManager::GetInstance()->SetDbKey(db_key);
}

static jint JNI_XWalkCookieManager_RekeyDb(JNIEnv* env, const JavaParamRef<_jobject*>& obj,
                                                   const JavaParamRef<_jstring*>& oldKey,
                                                   const JavaParamRef<_jstring*>& newKey) {
  std::string old_key(ConvertJavaStringToUTF8(env, oldKey));
  std::string new_key(ConvertJavaStringToUTF8(env, newKey));

  return CookieManager::GetInstance()->RekeyDb(old_key, new_key);
}

static void JNI_XWalkCookieManager_SetZone(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                   const JavaParamRef<jstring>& zone) {
  std::string zone_str(ConvertJavaStringToUTF8(env, zone));

  CookieManager::GetInstance()->SetZone(zone_str);
}

static jint JNI_XWalkCookieManager_NukeDomain(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                      const JavaParamRef<jstring>& domain) {
  std::string domain_str(ConvertJavaStringToUTF8(env, domain));

  return CookieManager::GetInstance()->NukeDomain(domain_str);
}

static void JNI_XWalkCookieManager_PageLoadStarted(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                           const JavaParamRef<jstring>& url) {
  std::string url_str(ConvertJavaStringToUTF8(env, url));

  CookieManager::GetInstance()->PageLoadStarted(url_str);
}

static void JNI_XWalkCookieManager_Reset(JNIEnv* env, const JavaParamRef<jobject>& obj) {
  CookieManager::GetInstance()->Reset();
}

// The following two methods are used to avoid a circular project dependency.
// TODO(mmenke):  This is weird. Maybe there should be a leaky Singleton in
// browser/net that creates and owns there?

scoped_refptr<base::SingleThreadTaskRunner> GetCookieStoreTaskRunner() {
  return CookieManager::GetInstance()->GetCookieStoreTaskRunner();
}

net::CookieStore* GetCookieStore() {
  return CookieManager::GetInstance()->GetCookieStore();
}

bool RegisterCookieManager(JNIEnv* env) {
  return false;
}

}  // namespace xwalk
