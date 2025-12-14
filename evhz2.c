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

unsigned long long get_time_us() {
    #ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (unsigned long long)((counter.QuadPart * 1000000ULL) / freq.QuadPart);
    #else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
    #endif
}

void update_hz(event_t* evt, unsigned long long time) {
    unsigned long long timediff = time - evt->prev_time;
    unsigned hz = 0;

    if (timediff != 0) {
        hz = 1000000ULL / timediff;
    }

    if (hz > 0 && hz < 10000) {
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
void windows_poll_loop(event_t* events) {
    POINT last_pos = {0, 0};
    int mouse_initialized = 0;

    strcpy(events[0].name, "Mouse");

    printf("Move your mouse. Press ESC to exit.\n\n");

    while (!quit) {
        if (_kbhit() && _getch() == 27) {
            quit = 1;
            break;
        }

        POINT pos;
        if (GetCursorPos(&pos)) {
            if (!mouse_initialized) {
                last_pos = pos;
                mouse_initialized = 1;
                events[0].prev_time = get_time_us();
            } else if (pos.x != last_pos.x || pos.y != last_pos.y) {
                unsigned long long time = get_time_us();
                update_hz(&events[0], time);
                last_pos = pos;
            }
        }

        Sleep(1);
    }

    if (events[0].avghz != 0) {
        printf("\nAverage for %s: %5dHz\n", events[0].name, events[0].avghz);
    }
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
                unsigned long long time = (unsigned long long)event.time.tv_sec * 1000000ULL;
                time += (unsigned long long)event.time.tv_usec;

                if (events[i].prev_time != 0) {
                    update_hz(&events[i], time);
                } else {
                    events[i].prev_time = time;
                }
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

void macos_input_callback(void* context, IOReturn result, void* sender, IOHIDValueRef value) {
    (void)context;
    (void)result;
    (void)sender;

    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t usage_page = IOHIDElementGetUsagePage(element);

    if (usage_page == kHIDPage_GenericDesktop) {
        unsigned long long time = get_time_us();

        if (macos_event.prev_time != 0) {
            update_hz(&macos_event, time);
        } else {
            macos_event.prev_time = time;
        }
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
    windows_poll_loop(events);
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
