"""Host regression tests for production readiness and mouse-task functions.

Requires Python 3 and GCC on PATH. The harness uses deterministic scheduler mocks,
not ESP-IDF, a real FreeRTOS scheduler, Bluetooth, or hardware. Production function
bodies are extracted unchanged; all generated files live in a temporary directory.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "main" / "main.c"


def production_excerpt(source):
    defines = []
    for name in ("MOUSE_ENCRYPTED_BIT", "MOUSE_HOST_ACTIVE_BIT", "MOUSE_READY_BITS"):
        match = re.search(r"^#define\s+" + name + r"\s+[^\n]+", source, re.MULTILINE)
        if match is None:
            raise ValueError(f"Missing production definition: {name}")
        defines.append(match.group())
    functions = []
    for name in ("mouse_is_ready", "wait_while_ready", "mouse_move_task", "hid_event_handler"):
        # The selected functions use a column-zero closing brace in main.c.
        match = re.search(
            r"^static (?:bool|void) " + name + r"\([^)]*\)\s*\n\{.*?^\}",
            source,
            re.MULTILINE | re.DOTALL,
        )
        if match is None:
            raise ValueError(f"Cannot extract production function: {name}")
        functions.append(match.group())
    return "\n\n".join(defines), "\n\n".join(functions)


HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define BIT0 1U
#define BIT1 2U
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define CONFIG_MOUSE_MOVE_INTERVAL_SEC 60
#define ESP_LOGI(...) ((void)0)
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #expr); \
    exit(1); } } while (0)

/* PRODUCTION_DEFINES */

typedef uint32_t EventBits_t;
typedef uint32_t TickType_t;
typedef struct { EventBits_t bits; } EventGroup;
typedef EventGroup *EventGroupHandle_t;
typedef struct { bool connected; } esp_hidd_dev_t;
typedef void *esp_event_base_t;
typedef enum {
    ESP_HIDD_START_EVENT, ESP_HIDD_CONNECT_EVENT, ESP_HIDD_PROTOCOL_MODE_EVENT,
    ESP_HIDD_CONTROL_EVENT, ESP_HIDD_DISCONNECT_EVENT
} esp_hidd_event_t;
typedef union {
    struct { int protocol_mode; } protocol_mode;
    struct { int control; } control;
} esp_hidd_event_data_t;
static EventGroup events;
static esp_hidd_dev_t device;
static EventGroupHandle_t s_mouse_events = &events;
static esp_hidd_dev_t *s_hid_device = &device;
static jmp_buf task_exit;
static unsigned wait_calls, delay_calls, movement_calls;
static unsigned advertising_requests;
static uint32_t elapsed_ms;
enum { BLOCKED = 1, YIELDED, MOVED, SPIN_GUARD };
enum { NORMAL, ASYNC_DISCONNECT, SUSPEND_DURING_WAIT };
static int scenario;

static EventBits_t xEventGroupGetBits(EventGroupHandle_t group)
{
    return group->bits;
}

static EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    return group->bits |= bits;
}

static EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t before = group->bits;
    group->bits &= ~bits;
    return before;
}

static void schedule_advertising(void)
{
    ++advertising_requests;
}

static bool esp_hidd_dev_connected(esp_hidd_dev_t *hid)
{
    return hid->connected;
}

static EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                      EventBits_t requested, int clear_on_exit,
                                      int wait_for_all, TickType_t timeout)
{
    CHECK(group != NULL);
    CHECK(requested == MOUSE_READY_BITS);
    CHECK(clear_on_exit == pdFALSE);
    CHECK(wait_for_all == pdTRUE);
    CHECK(timeout == portMAX_DELAY);
    if (++wait_calls > 100) {
        longjmp(task_exit, SPIN_GUARD);
    }
    if ((group->bits & requested) != requested) {
        /* A real scheduler would block this task, allowing other tasks to run. */
        longjmp(task_exit, BLOCKED);
    }
    return group->bits;
}

static void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks > 0);
    ++delay_calls;
    elapsed_ms += ticks;
    if (scenario == ASYNC_DISCONNECT) {
        longjmp(task_exit, YIELDED);
    }
    if (scenario == SUSPEND_DURING_WAIT && delay_calls == 1) {
        events.bits &= ~MOUSE_HOST_ACTIVE_BIT;
    }
}

static void mouse_move_square(void)
{
    ++movement_calls;
    longjmp(task_exit, MOVED);
}

/* PRODUCTION_FUNCTIONS */

static void test_readiness_matrix(void)
{
    for (unsigned bits = 0; bits < 4; ++bits) {
        for (unsigned has_events = 0; has_events < 2; ++has_events) {
            for (unsigned has_device = 0; has_device < 2; ++has_device) {
                for (unsigned connected = 0; connected < 2; ++connected) {
                    events.bits = bits;
                    s_mouse_events = has_events ? &events : NULL;
                    s_hid_device = has_device ? &device : NULL;
                    device.connected = connected;
                    bool expected = bits == MOUSE_READY_BITS && has_events &&
                                    has_device && connected;
                    CHECK(mouse_is_ready() == expected);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (strcmp(argv[1], "matrix") == 0) {
        test_readiness_matrix();
        return 0;
    }
    device.connected = true;
    events.bits = MOUSE_READY_BITS;
    esp_hidd_event_data_t hid_data = {0};
    if (strcmp(argv[1], "late_hid_disconnect") == 0) {
        /* A new GAP connection is ready before the previous HID event arrives. */
        hid_event_handler(NULL, NULL, ESP_HIDD_DISCONNECT_EVENT, &hid_data);
        CHECK(events.bits == MOUSE_READY_BITS);
        CHECK(mouse_is_ready());
        CHECK(advertising_requests == 0);
        return 0;
    } else if (strcmp(argv[1], "hid_suspend") == 0) {
        events.bits |= 4U;  /* Unrelated event bits must also be preserved. */
        hid_event_handler(NULL, NULL, ESP_HIDD_CONTROL_EVENT, &hid_data);
        CHECK(events.bits == (MOUSE_ENCRYPTED_BIT | 4U));
        CHECK(!mouse_is_ready());
        return 0;
    } else if (strcmp(argv[1], "hid_unencrypted_resume") == 0) {
        events.bits = 4U;
        hid_data.control.control = 1;
        hid_event_handler(NULL, NULL, ESP_HIDD_CONTROL_EVENT, &hid_data);
        CHECK(events.bits == (MOUSE_HOST_ACTIVE_BIT | 4U));
        CHECK(!mouse_is_ready());
        return 0;
    }
    int expected_stop = BLOCKED;
    if (strcmp(argv[1], "suspended") == 0) {
        events.bits = MOUSE_ENCRYPTED_BIT;
    } else if (strcmp(argv[1], "unencrypted_resume") == 0) {
        events.bits = MOUSE_HOST_ACTIVE_BIT;
    } else if (strcmp(argv[1], "async_disconnect") == 0) {
        device.connected = false;
        scenario = ASYNC_DISCONNECT;
        expected_stop = YIELDED;
    } else if (strcmp(argv[1], "suspend_during_wait") == 0) {
        scenario = SUSPEND_DURING_WAIT;
    } else if (strcmp(argv[1], "first_movement") == 0) {
        expected_stop = MOVED;
    } else {
        CHECK(false);
    }

    int stopped = setjmp(task_exit);
    if (stopped == 0) {
        mouse_move_task(NULL);
        CHECK(false);
    }
    CHECK(stopped == expected_stop);
    if (expected_stop == MOVED) {
        CHECK(movement_calls == 1);
        CHECK(elapsed_ms >= 3000);
    } else {
        CHECK(movement_calls == 0);
    }
    if (scenario == ASYNC_DISCONNECT) {
        CHECK(delay_calls == 1);
        CHECK(elapsed_ms > 0);
    } else if (scenario == SUSPEND_DURING_WAIT) {
        CHECK(wait_calls == 2);
        CHECK(elapsed_ms >= 250);
        CHECK(elapsed_ms < 3000);
    } else if (expected_stop == BLOCKED) {
        CHECK(wait_calls == 1);
        CHECK(delay_calls == 0);
    }
    return 0;
}
"""


class MouseReadinessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("gcc")
        if compiler is None:
            raise RuntimeError("GCC is required on PATH to run the host tests")
        defines, functions = production_excerpt(SOURCE.read_text(encoding="utf-8"))
        cls.temp = tempfile.TemporaryDirectory(prefix="mouse-readiness-")
        cls.addClassCleanup(cls.temp.cleanup)
        c_file = Path(cls.temp.name) / "harness.c"
        cls.executable = Path(cls.temp.name) / "harness.exe"
        c_file.write_text(
            HARNESS.replace("/* PRODUCTION_DEFINES */", defines).replace(
                "/* PRODUCTION_FUNCTIONS */", functions
            ),
            encoding="utf-8",
        )
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0",
             str(c_file), "-o", str(cls.executable)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def run_scenario(self, name):
        result = subprocess.run(
            [str(self.executable), name], capture_output=True, text=True, timeout=5
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_bits_and_hid_states(self):
        self.run_scenario("matrix")

    def test_suspended_encrypted_connection_blocks(self):
        self.run_scenario("suspended")

    def test_resume_without_encryption_blocks(self):
        self.run_scenario("unencrypted_resume")

    def test_asynchronous_hid_disconnect_yields(self):
        self.run_scenario("async_disconnect")

    def test_suspend_during_initial_wait_returns_to_blocking(self):
        self.run_scenario("suspend_during_wait")

    def test_first_movement_waits_three_seconds(self):
        self.run_scenario("first_movement")

    def test_late_hid_disconnect_preserves_new_connection(self):
        self.run_scenario("late_hid_disconnect")

    def test_hid_suspend_clears_only_host_active(self):
        self.run_scenario("hid_suspend")

    def test_hid_resume_does_not_restore_encryption(self):
        self.run_scenario("hid_unencrypted_resume")


if __name__ == "__main__":
    unittest.main()
