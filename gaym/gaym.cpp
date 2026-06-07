#include "stdafx.h"
#include "gaym.h"
#include "Dx12App.h"

#define MAX_LOADSTRING 100

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

Dx12App* g_pDx12App;

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // DPI awareness 설정 - 마우스/윈도우 좌표 일관성 보장
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_GAYM, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    g_pDx12App = new Dx12App();

    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_GAYM));

    MSG msg;

    while (true)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) break;
            if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            g_pDx12App->FrameAdvance();
        }
    }

    delete g_pDx12App;
    g_pDx12App = nullptr;

    // ServerCore static GCoreGlobal destructor 와 ucrtbase 의 stream 정리가 이 시점 이후
    //   서로 호출 순서가 뒤엉켜 SendBuffer 쪽에서 access violation / fatal exception 을 일으킴.
    //   우리 OnDestroy 가 NetworkManager / Scene / GPU / 폰트 / 오디오 자원을 모두 명시 정리했으니
    //   잔여 cleanup 은 OS 가 프로세스 자원 회수하게 맡김. TerminateProcess 는 가장 강력한 즉시
    //   종료 — C/C++ runtime cleanup 일체 skip, DLL_PROCESS_DETACH 도 정상 발생하지 않음.
    //   파일/소켓 핸들은 OS 가 자동 회수하므로 게임 종료에 부작용 없음.
    ::TerminateProcess(::GetCurrentProcess(), 0);

    return (int) msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_GAYM));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = NULL;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance;

   RECT rc = { 0, 0, kWindowWidth, kWindowHeight };
   DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_BORDER;
   AdjustWindowRect(&rc, dwStyle, FALSE);

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        g_pDx12App->OnCreate(hInst, hWnd);
        break;
    case WM_KEYDOWN:
        g_pDx12App->GetInputSystem().OnKeyDown(static_cast<int>(wParam));
        break;
    case WM_KEYUP:
        g_pDx12App->GetInputSystem().OnKeyUp(static_cast<int>(wParam));
        switch (wParam)
        {
        case VK_F11:
            g_pDx12App->ToggleFullscreen();
            break;
        }
        break;
    case WM_MOUSEMOVE:
        g_pDx12App->GetInputSystem().OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        break;
    case WM_MOUSEWHEEL:
        g_pDx12App->GetInputSystem().OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        break;
    case WM_LBUTTONDOWN:
        g_pDx12App->GetInputSystem().OnMouseButtonDown(0);
        break;
    case WM_LBUTTONUP:
        g_pDx12App->GetInputSystem().OnMouseButtonUp(0);
        break;
    case WM_RBUTTONDOWN:
        g_pDx12App->GetInputSystem().OnMouseButtonDown(1);
        break;
    case WM_RBUTTONUP:
        g_pDx12App->GetInputSystem().OnMouseButtonUp(1);
        break;
    case WM_MBUTTONDOWN:
        g_pDx12App->GetInputSystem().OnMouseButtonDown(2);
        break;
    case WM_MBUTTONUP:
        g_pDx12App->GetInputSystem().OnMouseButtonUp(2);
        break;
    case WM_SIZE:
        g_pDx12App->OnResize(LOWORD(lParam), HIWORD(lParam));
        break;
    case WM_ACTIVATEAPP:
        g_pDx12App->OnActivateApp(wParam != FALSE);
        break;
    case WM_DESTROY:
        g_pDx12App->OnDestroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}
