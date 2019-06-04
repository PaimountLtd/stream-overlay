#include "sl_overlays.h"
#include "stdafx.h"

#include <algorithm>
#include <iostream>

#include "sl_overlay_window.h"
#include "sl_overlays_settings.h"

#include "sl_overlay_api.h"
#include "overlay_logging.h"

#include "tlhelp32.h"
#pragma comment(lib, "uxtheme.lib")

#pragma comment(lib, "imm32.lib")

wchar_t const g_szWindowClass[] = L"overlays";
std::shared_ptr<smg_overlays> smg_overlays::instance = nullptr;

extern HANDLE overlays_thread;
extern DWORD overlays_thread_id;
extern std::mutex thread_state_mutex;
extern sl_overlay_thread_state thread_state;

BOOL CALLBACK get_overlayed_windows(HWND hwnd, LPARAM);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
bool FindRunningProcess(const WCHAR* process_name_part);

bool smg_overlays::process_commands(MSG& msg)
{
	bool ret = false;
	log_cout << "APP: process_commands id " << msg.wParam << std::endl;
	switch (msg.wParam)
	{
	case COMMAND_CATCH_APP:
	{
		HWND top_window = GetForegroundWindow();
		if (top_window != nullptr)
		{
			unsigned long process_id = 0;
			GetWindowThreadProcessId(top_window, &process_id);
			log_cout << "APP: catch app " << process_id << ", " << top_window << std::endl;
			EnumWindows(get_overlayed_windows, (LPARAM)&process_id);
		}
		ret = true;
	}
	break;
	case COMMAND_SHOW_OVERLAYS:
	{
		if (showing_overlays)
		{
			// need to hide befor show. or show can be ignored.
			showing_overlays = false;
			hide_overlays();
		}

		log_cout << "APP: show overlays " << std::endl;
		showing_overlays = true;
		ret = true;
	}
	break;
	case COMMAND_HIDE_OVERLAYS:
	{
		showing_overlays = false;

		hide_overlays();
		ret = true;
	}
	break;
	case COMMAND_UPDATE_OVERLAYS:
	{
		if (showing_overlays)
		{
			std::shared_lock<std::shared_mutex> lock(overlays_list_access);
			std::for_each(showing_windows.begin(), showing_windows.end(), [](std::shared_ptr<overlay_window>& n) {
				n->get_window_screenshot();
			});
		}
		ret = true;
	}
	break;

	case COMMAND_QUIT:
	{
		thread_state_mutex.lock();

		log_cout << "APP: COMMAND_QUIT " << (int)thread_state << std::endl;
		if (thread_state != sl_overlay_thread_state::runing)
		{
			thread_state_mutex.unlock();
		} else
		{
			thread_state = sl_overlay_thread_state::stopping;
			thread_state_mutex.unlock();
		}

		if (!quiting)
		{
			quit();
		}
		ret = true;
	}
	break;

	case COMMAND_TAKE_INPUT:
	{
		hook_user_input();
	}
	break;
	case COMMAND_RELEASE_INPUT:
	{
		unhook_user_input();
	}
	break;
	};

	return ret;
}

void smg_overlays::quit()
{
	log_cout << "APP: quit " << std::endl;
	quiting = true;

	update_settings();

	if (showing_windows.size() != 0)
	{
		std::for_each(showing_windows.begin(), showing_windows.end(), [](std::shared_ptr<overlay_window>& n) {
			PostMessage(0, WM_SLO_OVERLAY_CLOSE, n->id, 0);
		});
	} else
	{
		on_overlay_destroy(nullptr);
	}

	//it's not all. after last windows will be destroyed then thread quits
}

int smg_overlays::create_overlay_window_by_hwnd(HWND hwnd)
{
	std::shared_ptr<overlay_window> new_overlay_window = std::make_shared<overlay_window>();
	new_overlay_window->orig_handle = hwnd;
	//new_overlay_window->use_method = sl_window_capture_method::bitblt;
	new_overlay_window->get_window_screenshot();

	{
		std::unique_lock<std::shared_mutex> lock(overlays_list_access);
		showing_windows.push_back(new_overlay_window);
	}

	PostThreadMessage(
	    overlays_thread_id,
	    WM_SLO_HWND_SOURCE_READY,
	    new_overlay_window->id,
	    reinterpret_cast<LPARAM>(&(new_overlay_window->orig_handle)));

	return new_overlay_window->id;
}

void smg_overlays::on_update_timer()
{
	if (showing_overlays)
	{
		std::shared_lock<std::shared_mutex> lock(overlays_list_access);
		std::for_each(showing_windows.begin(), showing_windows.end(), [](std::shared_ptr<overlay_window>& n) {
			if (n->update_from_original && n->get_window_screenshot() || !n->update_from_original)
			{
				InvalidateRect(n->overlay_hwnd, nullptr, TRUE);
			}
		});
	}
}

void smg_overlays::deinit()
{
	log_cout << "APP: deinit " << std::endl;
}

void smg_overlays::hide_overlays()
{
	log_cout << "APP: hide_overlays " << std::endl;
	std::shared_lock<std::shared_mutex> lock(overlays_list_access);
	std::for_each(showing_windows.begin(), showing_windows.end(), [](std::shared_ptr<overlay_window>& n) {
		if (n->overlay_hwnd != 0)
		{
			ShowWindow(n->overlay_hwnd, SW_HIDE);
		}
	});
}

void smg_overlays::create_overlay_window_class()
{
	WNDCLASSEX wcex = {sizeof(wcex)};
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = GetModuleHandle(NULL);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszClassName = g_szWindowClass;

	RegisterClassEx(&wcex);
}

void smg_overlays::create_windows_overlays()
{
	std::shared_lock<std::shared_mutex> lock(overlays_list_access);
	std::for_each(showing_windows.begin(), showing_windows.end(), [this](std::shared_ptr<overlay_window>& n) {
		create_window_for_overlay(n);
	});
}

void smg_overlays::create_window_for_overlay(std::shared_ptr<overlay_window>& overlay)
{
	if (overlay->overlay_hwnd == nullptr && overlay->ready_to_create_overlay())
	{
		DWORD const dwStyle = WS_POPUP; // no border or title bar
		DWORD const dwStyleEx =
		    WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT; // transparent, topmost, with no taskbar
		                                                                          // item

		overlay->overlay_hwnd =
		    CreateWindowEx(dwStyleEx, g_szWindowClass, NULL, dwStyle, 0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);

		if (overlay->overlay_hwnd)
		{
			if (app_settings->use_color_key)
			{
				SetLayeredWindowAttributes(overlay->overlay_hwnd, RGB(0xFF, 0xFF, 0xFF), 0xD0, LWA_COLORKEY);
			} else
			{
				SetLayeredWindowAttributes(overlay->overlay_hwnd, RGB(0xFF, 0xFF, 0xFF), app_settings->transparency, LWA_ALPHA);
			}
			RECT overlay_rect = overlay->get_rect();
			SetWindowPos(
			    overlay->overlay_hwnd,
			    HWND_TOPMOST,
			    overlay_rect.left,
			    overlay_rect.top,
			    overlay_rect.right - overlay_rect.left,
			    overlay_rect.bottom - overlay_rect.top,
			    SWP_NOREDRAW);
			if (showing_overlays)
			{
				ShowWindow(overlay->overlay_hwnd, SW_SHOW);
			} else
			{
				ShowWindow(overlay->overlay_hwnd, SW_HIDE);
			}
		}
	}
}

HHOOK msg_hook = nullptr;
HHOOK llkeyboard_hook = nullptr;
HHOOK llmouse_hook = nullptr;

LRESULT CALLBACK CallWndMsgProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	log_cout << "APP: CallWndMsgProc " << wParam << std::endl;

	return CallNextHookEx(msg_hook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode >= 0)
	{
		KBDLLHOOKSTRUCT* event = (KBDLLHOOKSTRUCT*)lParam;
		log_cout << "APP: LowLevelKeyboardProc " << event->vkCode << ", " << event->dwExtraInfo << std::endl;

		if (event->vkCode == VK_ESCAPE)
		{
			use_callback_for_switching_input();
		} else

		use_callback_for_keyboard_input(wParam, lParam);
		return -1;
	}

	return CallNextHookEx(llkeyboard_hook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode >= 0)
	{
		MSLLHOOKSTRUCT* event = (MSLLHOOKSTRUCT*)lParam;
		log_cout << "APP: LowLevelMouseProc " << wParam << ", "<< event->pt.x << ", "<< event->pt.y << ", " << event->dwExtraInfo << std::endl;

		use_callback_for_mouse_input(wParam, lParam);
		if(wParam != WM_MOUSEMOVE)
		{
			return -1;
		}
	
	}
	return CallNextHookEx(llmouse_hook, nCode, wParam, lParam);
}

void smg_overlays::hook_user_input()
{
	log_cout << "APP: hook_user_input " << std::endl;

	if (!is_intercepting)
	{
		game_hwnd = GetForegroundWindow();
		if (game_hwnd != nullptr)
		{
			//print window title
			TCHAR title[256];
			GetWindowText(game_hwnd, title, 256);
			std::wstring title_wstr(title);
			std::string title_str(title_wstr.begin(), title_wstr.end());
			log_cout << "APP: hook_user_input catch window - " << title_str << std::endl;

			llkeyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
			llmouse_hook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);

			our_IMC = ImmCreateContext();
			if (our_IMC)
			{
				original_IMC = ImmAssociateContext(game_hwnd, our_IMC);
				if (!original_IMC)
				{
					game_hwnd = nullptr;
					ImmDestroyContext(our_IMC);
					our_IMC = nullptr;
				} else {
					is_intercepting = true;
				}
			} else
			{
				game_hwnd = nullptr;
			}
			is_intercepting = true;

			log_cout << "APP: Input hooked" << std::endl;
		}
	}
}

void smg_overlays::unhook_user_input()
{
	log_cout << "APP: unhook_user_input " << std::endl;
	if (is_intercepting)
	{
		if (msg_hook != nullptr)
		{
			UnhookWindowsHookEx(msg_hook);
			msg_hook = nullptr;
		}
		if (llkeyboard_hook != nullptr)
		{
			UnhookWindowsHookEx(llkeyboard_hook);
			llkeyboard_hook = nullptr;
		}
		if (llmouse_hook != nullptr)
		{
			UnhookWindowsHookEx(llmouse_hook);
			llmouse_hook = nullptr;
		}

		if (our_IMC)
		{
			ImmReleaseContext(game_hwnd, our_IMC);
			ImmDestroyContext(our_IMC);
			our_IMC = nullptr;
			game_hwnd = nullptr;
		}

		log_cout << "APP: Input unhooked" << std::endl;
		is_intercepting = false;
	}
}

void smg_overlays::original_window_ready(int overlay_id, HWND orig_window)
{
	std::shared_ptr<overlay_window> work_overlay = get_overlay_by_id(overlay_id);
	if (work_overlay != nullptr)
	{
		work_overlay->orig_handle = orig_window;
		create_window_for_overlay(work_overlay);
	}
}

void smg_overlays::create_windows_for_apps()
{
	std::for_each(app_settings->apps_names.begin(), app_settings->apps_names.end(), [](std::string& n) {
		WCHAR* process_name = new wchar_t[n.size() + 1];
		mbstowcs(&process_name[0], n.c_str(), n.size() + 1);

		FindRunningProcess(process_name);

		delete[] process_name;
	});
}

size_t smg_overlays::get_count()
{
	std::shared_lock<std::shared_mutex> lock(overlays_list_access);
	return showing_windows.size();
}

std::shared_ptr<overlay_window> smg_overlays::get_overlay_by_id(int overlay_id)
{
	std::shared_ptr<overlay_window> ret;
	std::shared_lock<std::shared_mutex> lock(overlays_list_access);

	std::list<std::shared_ptr<overlay_window>>::iterator findIter =
	    std::find_if(showing_windows.begin(), showing_windows.end(), [&overlay_id](std::shared_ptr<overlay_window>& n) {
		    return overlay_id == n->id;
	    });

	if (findIter != showing_windows.end())
	{
		ret = *findIter;
	}

	return ret;
}

std::shared_ptr<overlay_window> smg_overlays::get_overlay_by_window(HWND overlay_hwnd)
{
	std::shared_ptr<overlay_window> ret;
	std::shared_lock<std::shared_mutex> lock(overlays_list_access);

	std::list<std::shared_ptr<overlay_window>>::iterator findIter =
	    std::find_if(showing_windows.begin(), showing_windows.end(), [&overlay_hwnd](std::shared_ptr<overlay_window>& n) {
		    return overlay_hwnd == n->overlay_hwnd;
	    });

	if (findIter != showing_windows.end())
	{
		ret = *findIter;
	}

	return ret;
}

bool smg_overlays::remove_overlay(std::shared_ptr<overlay_window> overlay)
{
	log_cout << "APP: RemoveOverlay status " << (int)overlay->status << std::endl;
	if (overlay->status != overlay_status::destroing)
	{
		overlay->clean_resources();

		return true;
	}
	return false;
}

bool smg_overlays::on_window_destroy(HWND window)
{
	auto overlay = get_overlay_by_window(window);
	log_cout << "APP: on_window_destroy and overlay found " << (overlay != nullptr) << std::endl;
	bool removed = on_overlay_destroy(overlay);
	return removed;
}

bool smg_overlays::on_overlay_destroy(std::shared_ptr<overlay_window> overlay)
{
	bool removed = false;
	if (overlay != nullptr)
	{
		log_cout << "APP: overlay status was " << (int)overlay->status << std::endl;
		if (overlay->status == overlay_status::destroing)
		{
			std::unique_lock<std::shared_mutex> lock(overlays_list_access);
			showing_windows.remove_if([&overlay](std::shared_ptr<overlay_window>& n) { return (overlay->id == n->id); });
			removed = true;
		}
	}

	log_cout << "APP: overlays count " << showing_windows.size() << " and quiting " << quiting << std::endl;
	if (showing_windows.size() == 0 && quiting)
	{
		PostQuitMessage(0);
	} 

	return removed;
}

std::vector<int> smg_overlays::get_ids()
{
	std::vector<int> ret;
	int i = 0;
	std::shared_lock<std::shared_mutex> lock(overlays_list_access);
	ret.resize(showing_windows.size());
	std::for_each(showing_windows.begin(), showing_windows.end(), [&ret, &i](std::shared_ptr<overlay_window>& n) {
		ret[i] = n->id;
		i++;
	});

	return ret;
}

std::shared_ptr<smg_overlays> smg_overlays::get_instance()
{
	if (instance == nullptr)
	{
		instance = std::make_shared<smg_overlays>();
	}
	return instance;
}

smg_overlays::smg_overlays()
{
	showing_overlays = false;
	quiting = false;

	log_cout << "APP: start application " << std::endl;
}

void smg_overlays::init()
{
	app_settings->default_init();

	create_overlay_window_class();

	create_windows_for_apps();
}

bool FindRunningProcess(const WCHAR* process_name_part)
{
	bool procRunning = false;

	HANDLE hProcessSnap;
	PROCESSENTRY32 pe32;
	hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (hProcessSnap == INVALID_HANDLE_VALUE)
	{
		procRunning = false;
	} else
	{
		pe32.dwSize = sizeof(PROCESSENTRY32);
		if (Process32First(hProcessSnap, &pe32))
		{
			while (true)
			{
				if (StrStrW(pe32.szExeFile, process_name_part) != nullptr)
				{
					unsigned long process_id = pe32.th32ProcessID;
					EnumWindows(get_overlayed_windows, (LPARAM)&process_id);
				}
				if (!Process32Next(hProcessSnap, &pe32))
				{
					break;
				}
			}
			CloseHandle(hProcessSnap);
		}
	}

	return procRunning;
}

BOOL smg_overlays::process_found_window(HWND hwnd, LPARAM param)
{
	char buffer[128];
	bool window_ok = false;
	bool window_catched = false;
	std::shared_ptr<overlay_window> found_window = nullptr;
	DWORD dwProcessID = 0;
	if (param != NULL)
	{
		unsigned long process_id = 0;
		GetWindowThreadProcessId(hwnd, &process_id);
		dwProcessID = *( (unsigned long*)param);
		if ( dwProcessID == process_id && (GetWindow(hwnd, GW_OWNER) == (HWND) nullptr && IsWindowVisible(hwnd)))
		{
			window_ok = true;
		}
	} else
	{
		int written = GetWindowTextA(hwnd, buffer, 128);
		if (written && strstr(buffer, "Notepad.") != nullptr)
		{
			window_ok = true;
		}
	}

	if (window_ok)
	{
		WINDOWINFO wi = {0};
		GetWindowInfo(hwnd, &wi);
		int y = wi.rcWindow.bottom - wi.rcWindow.top;
		int x = wi.rcWindow.left - wi.rcWindow.right;

		int written = GetWindowTextA(hwnd, buffer, 128);
	}

	if (window_ok)
	{
		bool we_have_it = false;

		{
			std::shared_lock<std::shared_mutex> lock(overlays_list_access);
			std::for_each(
			    showing_windows.begin(), showing_windows.end(), [&hwnd, &we_have_it](std::shared_ptr<overlay_window>& n) {
				    if (n->orig_handle == hwnd)
				    {
					    we_have_it = true;
				    }
			    });
		}

		if (!we_have_it)
		{
			found_window = std::make_shared<overlay_window>();
			found_window->orig_handle = hwnd;
			found_window->get_window_screenshot();
			{
				std::unique_lock<std::shared_mutex> lock(overlays_list_access);
				showing_windows.push_back(found_window);
			}
			window_catched = true;

			// add process file name to settings
			TCHAR nameProcess[MAX_PATH];
			HANDLE processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwProcessID);
			DWORD file_name_size = MAX_PATH;
			QueryFullProcessImageName(processHandle, 0, nameProcess, &file_name_size);
			CloseHandle(processHandle);
			std::wstring ws(nameProcess);
			std::string temp_path(ws.begin(), ws.end());
			std::string::size_type pos = temp_path.find_last_of("\\/");
			std::string file_name = temp_path.substr(pos + 1, temp_path.size());

			std::list<std::string>::iterator findIter = std::find_if(
			    app_settings->apps_names.begin(), app_settings->apps_names.end(), [&file_name](const std::string& v) {
				    return v.compare(file_name) == 0;
			    });
			if (findIter == app_settings->apps_names.end())
			{
				app_settings->apps_names.push_back(file_name);
			}
		}
	}

	if (window_catched)
	{
		PostMessage(NULL, WM_SLO_SOURCE_CREATED, found_window->id, reinterpret_cast<LPARAM>(&(found_window->orig_handle)));
	}
	return TRUE;
}

void smg_overlays::draw_overlay_gdi(HWND& hWnd, bool g_bDblBuffered)
{
	PAINTSTRUCT ps;
	HPAINTBUFFER hBufferedPaint = NULL;
	RECT rc;

	GetClientRect(hWnd, &rc);
	HDC hdc = BeginPaint(hWnd, &ps);

	if (g_bDblBuffered)
	{
		// Get doublebuffered DC
		HDC hdcMem;
		hBufferedPaint = BeginBufferedPaint(hdc, &rc, BPBF_COMPOSITED, NULL, &hdcMem);
		if (hBufferedPaint)
		{
			hdc = hdcMem;
		}
	}

	{
		std::shared_lock<std::shared_mutex> lock(overlays_list_access);
		std::for_each(showing_windows.begin(), showing_windows.end(), [&hdc, &hWnd](std::shared_ptr<overlay_window>& n) {
			if (hWnd == n->overlay_hwnd)
			{
				RECT overlay_rect = n->get_rect();
				BOOL ret =true;

				ret = BitBlt(
				    hdc, 0, 0,
				    overlay_rect.right - overlay_rect.left,
				    overlay_rect.bottom - overlay_rect.top,
				    n->hdc, 0, 0, SRCCOPY);

				if (!ret)
				{
					log_cout << "APP: draw_overlay_gdi had issue " << GetLastError() << std::endl;
				}
			}
		});
	}

	if (hBufferedPaint)
	{
		// end painting
		BufferedPaintMakeOpaque(hBufferedPaint, nullptr);
		EndBufferedPaint(hBufferedPaint, TRUE);
	}

	EndPaint(hWnd, &ps);
}

void smg_overlays::update_settings()
{
	app_settings->web_pages.clear();

	std::shared_lock<std::shared_mutex> lock(overlays_list_access);
	std::for_each(showing_windows.begin(), showing_windows.end(), [this](std::shared_ptr<overlay_window>& n) {
		n->save_state_to_settings();
	});
	log_cout << "APP: update_settings finished " << std::endl;
}
