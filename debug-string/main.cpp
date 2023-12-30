#include <windows.h>
#include <CommCtrl.h>
#include <objidl.h>
#include <winioctl.h>

#include <iostream>

#pragma comment (lib,"Comctl32.lib")

#define FILE_DEVICE_IOCTL 0x00008301
#define IOCTL_REFERENCE_EVENT    CTL_CODE(FILE_DEVICE_IOCTL, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_DEREFERENCE_EVENT  CTL_CODE(FILE_DEVICE_IOCTL, 0x801, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IDM_ABOUT 32001
#define IDM_PROCESS_START 32002
#define IDM_PROCESS_ATTACH 32003

#define LISTVIEW_MESSAGES 32101

#define THE_BUFFER_LENGTH 512

typedef struct
{
	unsigned char buffer[THE_BUFFER_LENGTH];
	LARGE_INTEGER timestamp;
	unsigned int  number;
} MY_BUFFER, * PMY_BUFFER;

HINSTANCE hInst; 	
LPCTSTR szWindowClass = L"DSMONITOR";
LPCTSTR szTitle = L"Debug string monitor";

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

HWND ListCreate(HWND parent);
void ListAddMessage(LPWSTR message, LARGE_INTEGER timestamp,
	unsigned int number, bool isAnsi);

bool CreateCaptureService();
bool StartCaptureService();
void StopCaptureService();
void SendCallbackEvent();

DWORD WINAPI DriverDebugThread(LPVOID lpParam);
void DisplayDriverDbgString();
void DriverDebugLoop();

HANDLE g_signalEvent = NULL;
HANDLE g_hDevice = NULL;

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine,
	int nCmdShow)
{
	MSG msg;
	MyRegisterClass(hInstance);

	if (!InitInstance(hInstance, nCmdShow))
	{
		return FALSE;
	}
	
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW; 		
	wcex.lpfnWndProc = (WNDPROC)WndProc; 		
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance; 			
	wcex.hIcon = LoadIcon(NULL, IDI_HAND); 		
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW); 	
	wcex.hbrBackground = GetSysColorBrush(COLOR_WINDOW + 1); 
	wcex.lpszMenuName = NULL; 				
	wcex.lpszClassName = szWindowClass; 		
	wcex.hIconSm = NULL;

	return RegisterClassEx(&wcex); 			
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{

	HMENU hMenu = CreateMenu();
	HMENU hSubMenu = CreateMenu();
	AppendMenu(hSubMenu, MF_STRING, IDM_ABOUT, L"Про програму");

	AppendMenu(hMenu, MF_POPUP, (UINT)hSubMenu, L"Файл");

	HWND hWnd;
	hInst = hInstance; 
	hWnd = CreateWindow(szWindowClass, 	
		szTitle, 				
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 			
		CW_USEDEFAULT,			
		600, 					
		400, 					
		NULL, 					
		hMenu, 					
		hInstance, 				
		NULL); 				

	if (!hWnd) 	
	{
		return FALSE;
	}
	ShowWindow(hWnd, nCmdShow); 		
	UpdateWindow(hWnd); 				
	return TRUE;
}

HWND hList = 0;
DEBUG_EVENT g_debugEvent;

void ListAddMessage(LPWSTR message, LARGE_INTEGER timestamp,
	unsigned int number, bool isAnsi)
{
	FILETIME ft;
	SYSTEMTIME st;
	ft.dwHighDateTime = timestamp.HighPart;
	ft.dwLowDateTime = timestamp.LowPart;
	FileTimeToLocalFileTime(&ft, &ft);
	FileTimeToSystemTime(&ft, &st);

	WCHAR strTime[512];
	swprintf_s(strTime, L"%02d:%02d:%02d:%03d",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	LVITEM lvItem;

	lvItem.mask = LVIF_TEXT;
	lvItem.iItem = ListView_GetItemCount(hList);

	WCHAR strNumber[16];
	_itow_s(number, strNumber, 10);
	lvItem.pszText = strNumber;
	lvItem.iSubItem = 0;
	
	ListView_InsertItem(hList, &lvItem);

	lvItem.iSubItem = 1;
	lvItem.pszText = strTime;

	ListView_SetItem(hList, &lvItem);

	lvItem.iSubItem = 2;

	if (!isAnsi)
	{
		lvItem.pszText = message;
	}
	else
	{
		WCHAR wstr[512] = { 0 };
		MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED,
			(char*)message, strlen((char*)message), wstr, 512);
		lvItem.pszText = wstr;
	}

	ListView_SetItem(hList, &lvItem);
}

HWND ListCreate(HWND parent)
{
	INITCOMMONCONTROLSEX icex;
	icex.dwICC = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icex);
	RECT rcClient;

	GetClientRect(parent, &rcClient);

	HWND hWndListView = CreateWindow(WC_LISTVIEW,
		L"",
		WS_CHILD | LVS_REPORT,
		0, 0,
		rcClient.right - rcClient.left,
		rcClient.bottom - rcClient.top,
		parent,
		(HMENU)LISTVIEW_MESSAGES,
		hInst,
		NULL);

	LVCOLUMN lvc;

	WCHAR names[3][5] = {L"#", L"Time", L"Text"};
	UINT width[3] = { 30, 100, 470 };

	for (int i = 0; i < 3; i++)
	{
		lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		lvc.iSubItem = i;
		lvc.cx = width[i];
		lvc.pszText = names[i];
		lvc.fmt = LVCFMT_RIGHT;

		ListView_InsertColumn(hWndListView, i, &lvc);
	}

	ShowWindow(hWndListView, SW_SHOW);

	return hWndListView;
}

bool CreateCaptureService()
{
	LPCWSTR driverName = L"dbgprint-capture";
	LPCWSTR serviceExe = L"\\dbgprint-capture-driver.sys";

	wchar_t serviceExeFull[256];
	GetCurrentDirectory(256, serviceExeFull);
	wcscat_s(serviceExeFull, serviceExe);

	SC_HANDLE SCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	SC_HANDLE hService = NULL;
	if (SCManager == NULL)
	{
		MessageBox(NULL, L"Could not open SCManager", L"Error", MB_ICONERROR);
		return false;
	}

	SC_HANDLE service;
	service = CreateService(SCManager,
		driverName,           
		driverName,           
		SERVICE_ALL_ACCESS,   
		SERVICE_KERNEL_DRIVER,
		SERVICE_DEMAND_START,
		SERVICE_ERROR_NORMAL, 
		serviceExeFull,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL
	);
	if (service == NULL)
	{
		MessageBox(NULL, L"Could not create service", L"Error", MB_ICONERROR);
		return false;
	}
	CloseServiceHandle(hService);

	return true;
}

bool StartCaptureService()
{
	LPCWSTR driverName = L"dbgprint-capture";
	SC_HANDLE service;
	BOOL result = FALSE;

	SC_HANDLE SCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	service = OpenService(SCManager, driverName, SERVICE_ALL_ACCESS);

	if (service == NULL)
	{
		return FALSE;
	}

	result = StartService(service, 0, NULL)
		|| GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;

	CloseServiceHandle(service);

	return result;
}

void StopCaptureService()
{
	LPCWSTR driverName = L"dbgprint-capture";

	SC_HANDLE SCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	SC_HANDLE hService = NULL;
	if (SCManager == NULL)
	{
		MessageBox(NULL, L"Could not open SCManager", L"Error", MB_ICONERROR);
		return;
	}

	hService = OpenService(SCManager, driverName, SERVICE_ALL_ACCESS);
	if (hService == NULL)
	{
		MessageBox(NULL, L"Could not open service", L"Error", MB_ICONERROR);
		return;
	}

	SERVICE_STATUS serviceStatus;
	if (!ControlService(hService, SERVICE_CONTROL_STOP, &serviceStatus))
	{
		MessageBox(NULL, L"Could not stop service", L"Error", MB_ICONERROR);
		return;
	}
	DeleteService(hService);
	CloseServiceHandle(hService);
}

void SendCallbackEvent()
{
	g_hDevice = CreateFile(
	L"\\\\.\\LinkDbgPrintCapture",
	GENERIC_READ | GENERIC_WRITE,
	NULL,
	NULL,
	OPEN_EXISTING,
	NULL,
	NULL
	);

	if (g_hDevice == INVALID_HANDLE_VALUE)
	{
		wchar_t msg[32];
		swprintf_s(msg, L"Last Error: %X", GetLastError());
		MessageBox(NULL, msg, L"Could not open device", MB_ICONERROR);
	}

	g_signalEvent = CreateEvent(NULL, false, false, NULL);

	if (!DeviceIoControl(g_hDevice, IOCTL_REFERENCE_EVENT, (LPVOID)g_signalEvent,
		0, NULL, 0, NULL, NULL))
	{
		wchar_t msg[32];
		swprintf_s(msg, L"Last Error: %X", GetLastError());
		MessageBox(NULL, msg, L"DeviceIoControl failure", MB_ICONERROR);
	}
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CREATE:
	{
		hList = ListCreate(hWnd);
		if (!CreateCaptureService())
		{
			PostQuitMessage(0);
			break;
		}
		if(!StartCaptureService())
		{
			MessageBox(hWnd, L"Could not start service", L"", MB_ICONERROR);
			PostQuitMessage(0);
			break;
		}
		SendCallbackEvent();
		CreateThread(NULL, 0, DriverDebugThread, NULL, 0, NULL);
		break;
	}
	case WM_COMMAND:
	{
		switch (LOWORD(wParam))
		{
		case IDM_ABOUT:
			MessageBox(hWnd, L"Debug string moninor\n"
				L"Клієнтська програма для відображення\n"
				L"повідомлень DebugPrint та KdPrint\n"
				L"Виконав ст. гр. КІУКІ-21-3 Данилов Ілля",
				L"Про програму", MB_OK | MB_ICONINFORMATION);
			break;
		}
		break;
	}

	case WM_PAINT: 
		break;

	case WM_DESTROY:
		DeviceIoControl(g_hDevice, IOCTL_DEREFERENCE_EVENT, NULL,
			0, NULL, 0, NULL, NULL);
		CloseHandle(g_hDevice);
		StopCaptureService();
		PostQuitMessage(0);
		break;
	default:
		
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

void DisplayDriverDbgString()
{
	if (g_hDevice == INVALID_HANDLE_VALUE || g_hDevice == NULL)
		return;
	unsigned char buffer[sizeof(MY_BUFFER)] = {0};
	PMY_BUFFER wrapper = (PMY_BUFFER)buffer;
	ReadFile(g_hDevice, buffer, sizeof(MY_BUFFER), NULL, NULL);
	ListAddMessage((LPWSTR)wrapper->buffer, wrapper->timestamp,
		wrapper->number, true);
}

void DriverDebugLoop()
{
	while (1)
	{
		WaitForSingleObject(g_signalEvent, INFINITE);
		ResetEvent(g_signalEvent);
		DisplayDriverDbgString();
	}
}

DWORD WINAPI DriverDebugThread(LPVOID lpParam)
{
	DriverDebugLoop();
	return 0;
}