/* Copyright (C) 2016 Ian Kelling */
/* Licensed under the Apache License, Version 2.0 (the "License"); */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#define PLATFORM "Windows"
#elif defined(__linux__)
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#define PLATFORM "Linux"
#elif defined(__APPLE__) || defined(__MACH__)
#include <IOKit/hid/IOHIDManager.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#define PLATFORM "macOS"
#elif defined(__FreeBSD__)
#include <dev/evdev/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#define PLATFORM "FreeBSD"
#else
#error "Unsupported platform"
#endif

#define EVENTS 400
#define HZ_LIST 64

typedef struct event_s {
    #if defined(__linux__) || defined(__FreeBSD__)
    int fd;
    #endif
    int hz[HZ_LIST];
    int count;
    int avghz;
    unsigned long long prev_time;
    char name[128];
} event_t;

int quit = 0;
int verbose = 1;

void sigint_handler(int sig) {
    (void)sig;
    quit = 1;
}

void update_hz(event_t* evt, unsigned long long time) {
    if (evt->prev_time == 0) {
        evt->prev_time = time;
        return;
    }

    unsigned long long timediff = time - evt->prev_time;
    unsigned hz = 0;

    if (timediff != 0) {
        hz = 8000ULL / timediff;
    }

    if (hz > 0) {
        unsigned j, maxavg;

        evt->count++;
        evt->hz[evt->count & (HZ_LIST - 1)] = hz;
        evt->avghz = 0;

        maxavg = (evt->count > HZ_LIST) ? HZ_LIST : evt->count;

        for (j = 0; j < maxavg; j++) {
            evt->avghz += evt->hz[j];
        }

        evt->avghz /= maxavg;

        if (verbose) {
            printf("%s: Latest %5dHz, Average %5dHz\n",
                   evt->name, hz, evt->avghz);
        }
    }

    evt->prev_time = time;
}

#ifdef _WIN32
event_t win_mouse_event = {0};
event_t win_keyboard_event = {0};

LRESULT CALLBACK RawInputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        UINT size;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));

        LPBYTE buffer = (LPBYTE)malloc(size);
        if (buffer == NULL) {
            return 0;
        }

        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) {
            free(buffer);
            return 0;
        }

        RAWINPUT* raw = (RAWINPUT*)buffer;

        // Получаем timestamp из заголовка Raw Input (в единицах 125 микросекунд)
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);

        // Конвертируем в единицы 1/8000 секунды (как в Linux)
        unsigned long long time = (counter.QuadPart * 8000ULL) / freq.QuadPart;

        if (raw->header.dwType == RIM_TYPEMOUSE) {
            if (raw->data.mouse.lLastX != 0 || raw->data.mouse.lLastY != 0) {
                update_hz(&win_mouse_event, time);
            }
        } else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            if (raw->data.keyboard.Message == WM_KEYDOWN || raw->data.keyboard.Message == WM_KEYUP) {
                update_hz(&win_keyboard_event, time);
            }
        }

        free(buffer);
        return 0;
    } else if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void windows_poll_loop() {
    strcpy(win_mouse_event.name, "Mouse");
    strcpy(win_keyboard_event.name, "Keyboard");

    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = RawInputProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "RawInputClass";

    if (!RegisterClassEx(&wc)) {
        fprintf(stderr, "Failed to register window class\n");
        return;
    }

    HWND hwnd = CreateWindowEx(0, "RawInputClass", "RawInput", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);

    if (hwnd == NULL) {
        fprintf(stderr, "Failed to create window\n");
        return;
    }

    RAWINPUTDEVICE rid[2];

    // Mouse
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;

    // Keyboard
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = hwnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "Failed to register raw input devices\n");
        DestroyWindow(hwnd);
        return;
    }

    printf("Move your mouse or press keys. Press ESC to exit.\n\n");

    MSG msg;
    while (!quit) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (_kbhit()) {
            int ch = _getch();
            if (ch == 27) {
                quit = 1;
            }
        }

        Sleep(1);
    }

    if (win_mouse_event.avghz != 0) {
        printf("\nAverage for %s: %5dHz\n", win_mouse_event.name, win_mouse_event.avghz);
    }
    if (win_keyboard_event.avghz != 0) {
        printf("Average for %s: %5dHz\n", win_keyboard_event.name, win_keyboard_event.avghz);
    }

    DestroyWindow(hwnd);
}
#endif

#if defined(__linux__) || defined(__FreeBSD__)
void linux_poll_loop(event_t* events, int max_event) {
    while (!quit) {
        fd_set set;
        FD_ZERO(&set);

        for (int i = 0; i <= max_event; i++) {
            if (events[i].fd != -1) {
                FD_SET(events[i].fd, &set);
            }
        }

        if (select(FD_SETSIZE, &set, NULL, NULL, NULL) <= 0) {
            continue;
        }

        struct input_event event;

        for (int i = 0; i <= max_event; i++) {
            if (events[i].fd == -1 || !FD_ISSET(events[i].fd, &set)) {
                continue;
            }

            int bytes = read(events[i].fd, &event, sizeof(event));

            if (bytes != sizeof(event)) {
                continue;
            }

            if (event.type == EV_REL || event.type == EV_ABS) {
                // Конвертируем в единицы 1/8000 секунды (как в оригинале)
                unsigned long long time = (unsigned long long)event.time.tv_sec * 8000ULL;
                time += (unsigned long long)event.time.tv_usec / 125ULL;

                update_hz(&events[i], time);
            }
        }
    }

    for (int i = 0; i <= max_event; i++) {
        if (events[i].fd != -1) {
            if (events[i].avghz != 0) {
                printf("\nAverage for %s: %5dHz\n", events[i].name, events[i].avghz);
            }
            close(events[i].fd);
        }
    }
}
#endif

#if defined(__APPLE__) || defined(__MACH__)
event_t macos_event = {0};

unsigned long long macos_get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long time = (unsigned long long)ts.tv_sec * 8000ULL;
    time += (unsigned long long)ts.tv_nsec / 125000ULL;
    return time;
}

void macos_input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value) {
    (void)context;
    (void)result;
    (void)sender;

    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t usage_page = IOHIDElementGetUsagePage(element);

    if (usage_page == kHIDPage_GenericDesktop) {
        unsigned long long time = macos_get_time();
        update_hz(&macos_event, time);
    }
}

void macos_poll_loop() {
    strcpy(macos_event.name, "HID Device");

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);

    if (!manager) {
        fprintf(stderr, "Failed to create HID manager\n");
        return;
    }

    IOHIDManagerSetDeviceMatching(manager, NULL);
    IOHIDManagerRegisterInputValueCallback(manager, macos_input_callback, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    IOReturn ret = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "Failed to open HID manager\n");
        CFRelease(manager);
        return;
    }

    printf("Move your mouse or use keyboard. Press CTRL-C to exit.\n\n");

    while (!quit) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    }

    if (macos_event.avghz != 0) {
        printf("\nAverage for %s: %5dHz\n", macos_event.name, macos_event.avghz);
    }

    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
}
#endif

int main(int argc, char* argv[]) {
    printf("Event Hz Tester - %s\n", PLATFORM);
    printf("====================\n\n");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--nonverbose") == 0) {
            verbose = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-n|-h]\n", argv[0]);
            printf("  -n, --nonverbose    Nonverbose mode\n");
            printf("  -h, --help          Show this help\n");
            return 0;
        }
    }

    #ifndef _WIN32
    if (geteuid() != 0) {
        printf("Warning: %s should be run as superuser for full access\n\n", argv[0]);
    }
    #endif

    signal(SIGINT, sigint_handler);

    event_t events[EVENTS];
    memset(events, 0, sizeof(events));

    #ifdef _WIN32
    windows_poll_loop();
    #elif defined(__APPLE__) || defined(__MACH__)
    macos_poll_loop();
    #elif defined(__linux__) || defined(__FreeBSD__)
    int max_event = -1;

    for (int i = 0; i < EVENTS; i++) {
        char device[30];
        sprintf(device, "/dev/input/event%d", i);
        events[i].fd = open(device, O_RDONLY);

        if (events[i].fd != -1) {
            max_event = i;
            ioctl(events[i].fd, EVIOCGNAME(sizeof(events[i].name)), events[i].name);
            if (verbose) {
                printf("event%d: %s\n", i, events[i].name);
            }
        }
    }

    if (max_event == -1) {
        fprintf(stderr, "No input devices found\n");
        return 1;
    }

    printf("\nPress CTRL-C to exit.\n\n");
    linux_poll_loop(events, max_event);
    #endif

    return 0;
}
