#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <include/wrapper/cef_helpers.h>
#include <include/cef_app.h>
#include <unistd.h>
#include <gdk/gdkx.h>
#include <gtk/gtkgl.h>
#include <sys/file.h>

#include "brick_app.h"
#include "third-party/json/json.h"
#include "status_icon/status_icon.h"
#include "cef_handler.h"
#include "cef_app.h"
#include "window_util.h"
#include "helper.h"
#include "platform_util.h"


#undef Status   // Definition conflicts with cef_urlrequest.h
#undef Success  // Definition conflicts with cef_message_router.h

namespace {
    std::string APPICONS[] = {"brick16.png", "brick32.png", "brick48.png", "brick128.png", "brick256.png"};
    std::string szWorkingDir;  // The current working directory

    bool
    GetWorkingDir(std::string& dir) {
      char buff[1024];

      // Retrieve the executable path.
      ssize_t len = readlink("/proc/self/exe", buff, sizeof(buff)-1);
      if (len == -1)
        return false;

      buff[len] = 0;
      dir = std::string(buff);
      // Remove the executable name from the path.
      dir = dir.substr(0, dir.find_last_of("/"));

      return true;
    }

    bool
    EnsureSystemDirectoryExists() {
      return (
         platform_util::MakeDirectory(std::string(BrickApp::GetConfigHome()) + "/" + APP_COMMON_NAME)
          && platform_util::MakeDirectory(std::string(BrickApp::GetCacheHome()) + "/" + APP_COMMON_NAME)
      );
    }

    bool
    EnsureSingleInstance() {
      // ToDo: Replaced by IPC request, when IPC will be implemented
      std::string lock_file = std::string(BrickApp::GetCacheHome()) + "/" + APP_COMMON_NAME + "/run.lock";
      int fd = open(lock_file.c_str(), O_CREAT, 0600);
      if (fd == -1)
        return true;

      return flock(fd, LOCK_EX|LOCK_NB) == 0;
    }

    gboolean
    CheckUserIdle(gpointer data) {
      /* Query xscreensaver */
      static XScreenSaverInfo *mit_info = NULL;
      static int has_extension = -1;
      static bool idle_state = false;
      int event_base, error_base;

      if (has_extension == -1)
        has_extension = XScreenSaverQueryExtension(GDK_DISPLAY(), &event_base, &error_base);

      if (!has_extension)
        return false; // Don't call this function any more

      CefRefPtr<ClientHandler> handler = ClientHandler::GetInstance();
      if (!handler)
        return true;

      CefRefPtr<CefBrowser> browser = handler->GetBrowser();
      if (!browser)
        return true;

      if (mit_info == NULL)
        mit_info = XScreenSaverAllocInfo();

      XScreenSaverQueryInfo(GDK_DISPLAY(), GDK_ROOT_WINDOW(), mit_info);

      if (mit_info->idle >= IDLE_TIMEOUT && !idle_state) {
        idle_state = true;
        handler->SendJSEvent(browser, "BXUserAway", "[true]");
      } else if (mit_info->idle < IDLE_TIMEOUT && idle_state) {
        idle_state = false;
        handler->SendJSEvent(browser, "BXUserAway", "[false]");
      }

      return true;
    }
}

// static
const char*
BrickApp::GetConfigHome() {
  return g_get_user_config_dir();
}

// static
const char*
BrickApp::GetCacheHome() {
  return g_get_user_cache_dir();
}

void
TerminationSignalHandler(int signatl) {
  CefQuitMessageLoop();
}

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);
  CefRefPtr<ClientApp> app(new ClientApp);

  // Execute the secondary process, if any.
  int exit_code = CefExecuteProcess(main_args, app.get(), NULL);
  if (exit_code >= 0)
    return exit_code;

  if (!EnsureSystemDirectoryExists()) {
    printf("Can't create system directories");
    return 1;
  }

  if (!EnsureSingleInstance()) {
    // ToDo: change main window stack order, when IPC will be implemented
    printf("Another instance is already running.\nExiting.");
    return 0;
  }


  GetWorkingDir(szWorkingDir);
  std::string plain_config = BrickApp::GetConfig();
  AppSettings app_settings = AppSettings::InitByJson(plain_config);
  app_settings.resource_dir = helper::BaseDir(szWorkingDir) + "/resources/";

  CefRefPtr<AccountManager> account_manager(new AccountManager);
  // ToDo: Fix this bullshit!
  account_manager->Init(
     std::string(BrickApp::GetConfigHome()) + "/" + APP_COMMON_NAME + "/accounts.json"
  );

  // Initialize CEF.
  CefInitialize(main_args, BrickApp::GetCefSettings(szWorkingDir, app_settings), app.get(), NULL);

  gtk_init(0, NULL);
  // Perform gtkglext initialization required by the OSR.
  gtk_gl_init(0, NULL);
  gtk_timeout_add(2000, CheckUserIdle, NULL);
  window_util::InitHooks();

  // Set default windows icon. Important to do this before any GTK window created!
  GList *icons = NULL;
  std::string icon_path = app_settings.resource_dir + "/app_icons/";
  int icons_count = sizeof(APPICONS) / sizeof(APPICONS[0]);
  for (int i = 0; i < icons_count; ++i) {
    GdkPixbuf *icon = gdk_pixbuf_new_from_file((icon_path + APPICONS[i]).c_str(), NULL);
    if (!icon)
      continue;
    icons = g_list_append(icons, icon);
  }
  gtk_window_set_default_icon_list(icons);
  g_list_foreach(icons, (GFunc) g_object_unref, NULL);
  g_list_free(icons);

  // Create main window
  CefRefPtr<BrowserWindow> main_window(new BrowserWindow);
  main_window->Init();
  main_window->Show();
  // Create the handler.
  CefRefPtr<ClientHandler> client_handler(new ClientHandler);
  client_handler->SetMainWindowHandle(main_window);
  client_handler->SetAppSettings(app_settings);
  client_handler->SetAccountManager(account_manager);

  // Initialize status icon
  CefRefPtr<StatusIcon> status_icon(new StatusIcon(app_settings.resource_dir + "/indicators/"));
  client_handler->SetStatusIconHandle(status_icon);

  CefWindowInfo window_info;
  window_info.SetAsWindowless(main_window->GetId(), false);
  std::string startup_url = account_manager->GetCurrentAccount()->GetBaseUrl();
  if (account_manager->GetCurrentAccount()->IsExisted()) {
    // Login to our account
    startup_url += "internals/pages/portal-loader#login=yes";
  } else {
    // Otherwise let's show error page
    startup_url += "internals/pages/home";
  }

  // Create browser
  CefBrowserHost::CreateBrowserSync(
     window_info, client_handler.get(),
     startup_url, BrickApp::GetBrowserSettings(szWorkingDir, app_settings), NULL);

  // Install a signal handler so we clean up after ourselves.
  signal(SIGINT, TerminationSignalHandler);
  signal(SIGTERM, TerminationSignalHandler);

  CefRunMessageLoop();

  CefShutdown();

  return 0;
}
