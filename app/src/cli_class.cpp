#include "cli_class.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"         // for esp_restart
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "time_sync.h"

namespace
{
    constexpr int MAX_ARGS = 8;

    // Seconds since an esp_timer timestamp, or -1 if there has been no reading
    long age_s(int64_t ts_us)
    {
        if (ts_us == 0) { return -1; }
        return static_cast<long>((esp_timer_get_time() - ts_us) / 1000000);
    }

    // snprintf that appends and never overruns; returns the new length
    size_t appendf(char* out, size_t len, size_t pos, const char* fmt, ...)
    {
        if (pos >= len) { return pos; }
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(out + pos, len - pos, fmt, ap);
        va_end(ap);
        if (n < 0) { return pos; }
        return (pos + n < len) ? pos + n : len - 1;
    }

    int reply(char* out, size_t len, const char* text)
    {
        snprintf(out, len, "%s", text);
        return 0;
    }

    int error(char* out, size_t len, const char* text)
    {
        snprintf(out, len, "error: %s", text);
        return 1;
    }

    bool parse_float(const char* s, float& v)
    {
        char* end = nullptr;
        v = strtof(s, &end);
        return end != s && *end == '\0';
    }

    // Delayed so the reply (and an MQTT response) gets out first
    void reboot_task(void*)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    const char* HELP =
        "help\n"
        "status                      every subsystem on one line each\n"
        "dht read|status\n"
        "water read|status|rom\n"
        "ph read|status|temp <degC>|cal mid|low|high <value>|cal clear|cal ?\n"
        "orp read|status|cal <mV>|cal clear|cal ?\n"
        "wifi status|ps on|off\n"
        "mqtt status\n"
        "queue status|push <id> <value>   push is a real reading: the server stores it\n"
        "time\n"
        "seq\n"
        "reboot";
}

Cli::Cli(const Targets& targets):
    _t(targets)
{
}

// ---------------------------------------------------------------- dispatcher

int Cli::execute(const char* line, char* out, size_t len)
{
    if (!out || len == 0) { return 1; }
    out[0] = '\0';
    if (!line) { return error(out, len, "empty"); }

    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s", line);

    char* argv[MAX_ARGS];
    int argc = 0;
    for (char* tok = strtok(buf, " \t\r\n"); tok && argc < MAX_ARGS; tok = strtok(nullptr, " \t\r\n"))
    {
        argv[argc++] = tok;
    }
    if (argc == 0) { return 0; }   // blank line

    const char* cmd = argv[0];
    if (!strcmp(cmd, "help"))   { return cmd_help(argc, argv, out, len); }
    if (!strcmp(cmd, "status")) { return cmd_status(argc, argv, out, len); }
    if (!strcmp(cmd, "dht"))    { return cmd_dht(argc, argv, out, len); }
    if (!strcmp(cmd, "water"))  { return cmd_water(argc, argv, out, len); }
    if (!strcmp(cmd, "ph"))     { return cmd_ezo(_t.ph,  "ph",  "PH",  argc, argv, out, len); }
    if (!strcmp(cmd, "orp"))    { return cmd_ezo(_t.orp, "orp", "ORP", argc, argv, out, len); }
    if (!strcmp(cmd, "wifi"))   { return cmd_wifi(argc, argv, out, len); }
    if (!strcmp(cmd, "mqtt"))   { return cmd_mqtt(argc, argv, out, len); }
    if (!strcmp(cmd, "queue"))  { return cmd_queue(argc, argv, out, len); }
    if (!strcmp(cmd, "time"))   { return cmd_time(argc, argv, out, len); }
    if (!strcmp(cmd, "seq"))    { return cmd_seq(argc, argv, out, len); }
    if (!strcmp(cmd, "reboot"))
    {
        xTaskCreate(reboot_task, "reboot", 2048, nullptr, 1, nullptr);
        return reply(out, len, "rebooting in 1 s");
    }

    snprintf(out, len, "error: unknown command '%s', try help", cmd);
    return 1;
}

// ---------------------------------------------------------------- commands

int Cli::cmd_help(int, char**, char* out, size_t len)
{
    return reply(out, len, HELP);
}

int Cli::cmd_status(int, char**, char* out, size_t len)
{
    size_t pos = 0;

    if (_t.wifi) {
        pos = appendf(out, len, pos, "wifi %s ip=%s disconnects=%lu\n",
                      status_name(_t.wifi->get_status()), _t.wifi->has_ip() ? "yes" : "no",
                      _t.wifi->get_disconnect_count());
    }
    if (_t.mqtt) {
        pos = appendf(out, len, pos, "mqtt %s\n", status_name(_t.mqtt->get_status()));
    }

    time_t now = time(nullptr);
    pos = appendf(out, len, pos, "time synced=%s epoch=%lld\n",
                  time_is_synced() ? "yes" : "no", static_cast<long long>(now));

    if (_t.seq)   { pos = appendf(out, len, pos, "seq next=%lu\n", _t.seq->peek()); }
    if (_t.queue) { pos = appendf(out, len, pos, "queue %u waiting, %lu dropped\n",
                                  static_cast<unsigned>(_t.queue->count()), _t.queue->get_dropped()); }

    if (_t.dht) {
        auto d = _t.dht->read_data();
        pos = appendf(out, len, pos, "dht %s %.1fC %.1f%%RH age=%lds\n",
                      status_name(_t.dht->get_status()), d.temperature, d.humidity, age_s(d.timestamp));
    }
    if (_t.water) {
        auto w = _t.water->read_data();
        pos = appendf(out, len, pos, "water %s %.2fC age=%lds\n",
                      status_name(_t.water->get_status()), w.temperature, age_s(w.timestamp));
    }
    if (_t.ph) {
        auto p = _t.ph->read_data();
        pos = appendf(out, len, pos, "ph %s %.2f age=%lds\n",
                      status_name(_t.ph->get_status()), p.value, age_s(p.timestamp));
    }
    if (_t.orp) {
        auto o = _t.orp->read_data();
        pos = appendf(out, len, pos, "orp %s %.0fmV age=%lds\n",
                      status_name(_t.orp->get_status()), o.value, age_s(o.timestamp));
    }

    // drop the trailing newline
    if (pos > 0 && out[pos - 1] == '\n') { out[pos - 1] = '\0'; }
    return 0;
}

int Cli::cmd_dht(int argc, char** argv, char* out, size_t len)
{
    if (!_t.dht) { return error(out, len, "dht not present"); }
    if (argc < 2) { return error(out, len, "usage: dht read|status"); }

    if (!strcmp(argv[1], "status"))
    {
        auto d = _t.dht->read_data();
        snprintf(out, len, "dht %s last=%.1fC %.1f%%RH age=%lds",
                 status_name(_t.dht->get_status()), d.temperature, d.humidity, age_s(d.timestamp));
        return 0;
    }
    if (!strcmp(argv[1], "read"))
    {
        // The driver enforces its own 2 s minimum interval, so a read right
        // after the sampler's comes back SENSOR_NOT_READY; that is expected
        DhtSensor::ReturnCode rc;
        { SensorLock lock; rc = _t.dht->sample(); }
        if (rc != DhtSensor::ReturnCode::DHT_OK)
        {
            snprintf(out, len, "error: dht sample failed rc=%d", static_cast<int>(rc));
            return 1;
        }
        auto d = _t.dht->read_data();
        snprintf(out, len, "dht %.1fC %.1f%%RH", d.temperature, d.humidity);
        return 0;
    }
    return error(out, len, "usage: dht read|status");
}

int Cli::cmd_water(int argc, char** argv, char* out, size_t len)
{
    if (!_t.water) { return error(out, len, "water not present"); }
    if (argc < 2) { return error(out, len, "usage: water read|status|rom"); }

    if (!strcmp(argv[1], "status"))
    {
        auto w = _t.water->read_data();
        snprintf(out, len, "water %s last=%.2fC age=%lds",
                 status_name(_t.water->get_status()), w.temperature, age_s(w.timestamp));
        return 0;
    }
    if (!strcmp(argv[1], "rom"))
    {
        snprintf(out, len, "water rom=%016llX", _t.water->get_rom());
        return 0;
    }
    if (!strcmp(argv[1], "read"))
    {
        Ds18b20::ReturnCode rc;
        { SensorLock lock; rc = _t.water->sample(); }
        if (rc != Ds18b20::ReturnCode::OK)
        {
            snprintf(out, len, "error: water sample failed rc=%d", static_cast<int>(rc));
            return 1;
        }
        snprintf(out, len, "water %.2fC", _t.water->read_data().temperature);
        return 0;
    }
    return error(out, len, "usage: water read|status|rom");
}

int Cli::cmd_ezo(EzoSensor* s, const char* name, const char* id, int argc, char** argv, char* out, size_t len)
{
    if (!s) { snprintf(out, len, "error: %s not present", name); return 1; }
    if (argc < 2)
    {
        snprintf(out, len, "usage: %s read|status|temp <degC>|cal <args>|cal clear|cal ?", name);
        return 1;
    }

    if (!strcmp(argv[1], "status"))
    {
        auto d = s->read_data();
        snprintf(out, len, "%s %s addr=0x%02X last=%.2f age=%lds",
                 name, status_name(s->get_status()), s->get_address(), d.value, age_s(d.timestamp));
        return 0;
    }
    if (!strcmp(argv[1], "read"))
    {
        EzoSensor::ReturnCode rc;
        { SensorLock lock; rc = s->sample(); }
        if (rc != EzoSensor::ReturnCode::OK)
        {
            snprintf(out, len, "error: %s sample failed rc=%d", name, static_cast<int>(rc));
            return 1;
        }
        snprintf(out, len, "%s %.2f", name, s->read_data().value);
        return 0;
    }
    if (!strcmp(argv[1], "temp"))
    {
        float c;
        if (argc < 3 || !parse_float(argv[2], c)) { return error(out, len, "usage: temp <degC>"); }
        EzoSensor::ReturnCode rc;
        { SensorLock lock; rc = s->set_temperature(c); }
        if (rc != EzoSensor::ReturnCode::OK)
        {
            snprintf(out, len, "error: %s temp failed rc=%d", name, static_cast<int>(rc));
            return 1;
        }
        snprintf(out, len, "%s temp=%.2f", name, c);
        return 0;
    }
    if (!strcmp(argv[1], "cal"))
    {
        if (argc < 3) { return error(out, len, "usage: cal <args>|cal clear|cal ?"); }

        if (!strcmp(argv[2], "?"))
        {
            int points = 0;
            EzoSensor::ReturnCode rc;
            { SensorLock lock; rc = s->calibration_points(points); }
            if (rc != EzoSensor::ReturnCode::OK)
            {
                snprintf(out, len, "error: %s cal ? failed rc=%d", name, static_cast<int>(rc));
                return 1;
            }
            snprintf(out, len, "%s cal points=%d", name, points);
            return 0;
        }

        // "cal mid 7.00" -> "mid,7.00"; "cal 225" -> "225"; "cal clear" -> "clear"
        char args[32] = {};
        size_t pos = 0;
        for (int i = 2; i < argc; i++)
        {
            pos = appendf(args, sizeof(args), pos, "%s%s", i > 2 ? "," : "", argv[i]);
        }

        EzoSensor::ReturnCode rc;
        { SensorLock lock; rc = s->calibrate(args); }
        if (rc != EzoSensor::ReturnCode::OK)
        {
            snprintf(out, len, "error: %s cal %s failed rc=%d", name, args, static_cast<int>(rc));
            return 1;
        }

        int points = -1;
        { SensorLock lock; s->calibration_points(points); }   // best effort, for the reply

        // Durable record of the calibration (POOL-10/11): the stamp keeps no
        // history itself. Retained, so the server has the latest even after a
        // restart. ts is left out when the clock isn't synced.
        char leaf[40];
        char when[32] = "";
        char payload[96];
        if (time_is_synced()) {
            snprintf(when, sizeof(when), "\"ts\":%lld,", static_cast<long long>(time(nullptr)));
        }
        snprintf(leaf, sizeof(leaf), "calibration/%s", id);
        snprintf(payload, sizeof(payload), "{%s\"cmd\":\"%s\",\"points\":%d}", when, args, points);
        publish_sysinfo(_t.mqtt, _t.sysinfo_prefix, leaf, payload);

        snprintf(out, len, "%s calibrated %s points=%d", name, args, points);
        return 0;
    }
    snprintf(out, len, "usage: %s read|status|temp <degC>|cal <args>|cal clear|cal ?", name);
    return 1;
}

int Cli::cmd_wifi(int argc, char** argv, char* out, size_t len)
{
    if (!_t.wifi) { return error(out, len, "wifi not present"); }
    if (argc < 2) { return error(out, len, "usage: wifi status|ps on|off"); }

    if (!strcmp(argv[1], "status"))
    {
        snprintf(out, len, "wifi %s ip=%s disconnects=%lu",
                 status_name(_t.wifi->get_status()), _t.wifi->has_ip() ? "yes" : "no",
                 _t.wifi->get_disconnect_count());
        return 0;
    }
    if (!strcmp(argv[1], "ps"))
    {
        if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off")))
        {
            return error(out, len, "usage: wifi ps on|off");
        }
        bool on = !strcmp(argv[2], "on");
        if (_t.wifi->set_power_save(on) != Wifi::ReturnCode::OK) { return error(out, len, "set power save failed"); }
        snprintf(out, len, "wifi ps=%s", on ? "on" : "off");
        return 0;
    }
    return error(out, len, "usage: wifi status|ps on|off");
}

int Cli::cmd_mqtt(int argc, char** argv, char* out, size_t len)
{
    if (!_t.mqtt) { return error(out, len, "mqtt not present"); }
    if (argc < 2 || strcmp(argv[1], "status")) { return error(out, len, "usage: mqtt status"); }

    snprintf(out, len, "mqtt %s connected=%s",
             status_name(_t.mqtt->get_status()), _t.mqtt->is_connected() ? "yes" : "no");
    return 0;
}

int Cli::cmd_queue(int argc, char** argv, char* out, size_t len)
{
    if (!_t.queue) { return error(out, len, "queue not present"); }
    if (argc < 2) { return error(out, len, "usage: queue status|push <id> <value>"); }

    if (!strcmp(argv[1], "status"))
    {
        snprintf(out, len, "queue %u waiting, %lu dropped",
                 static_cast<unsigned>(_t.queue->count()), _t.queue->get_dropped());
        return 0;
    }
    if (!strcmp(argv[1], "push"))
    {
        float v;
        if (argc < 4 || !parse_float(argv[3], v)) { return error(out, len, "usage: queue push <id> <value>"); }
        if (!time_is_synced()) { return error(out, len, "time not synced, reading would carry a bogus ts"); }

        _t.queue->push(argv[2], time(nullptr), v);
        snprintf(out, len, "queued %s=%.2f, publishes on the next transmit", argv[2], v);
        return 0;
    }
    return error(out, len, "usage: queue status|push <id> <value>");
}

int Cli::cmd_time(int, char**, char* out, size_t len)
{
    time_t now = time(nullptr);
    char utc[32] = "unsynced";
    if (time_is_synced())
    {
        struct tm t;
        gmtime_r(&now, &t);
        strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &t);
    }
    snprintf(out, len, "time synced=%s epoch=%lld utc=%s uptime=%llds",
             time_is_synced() ? "yes" : "no", static_cast<long long>(now), utc,
             static_cast<long long>(esp_timer_get_time() / 1000000));
    return 0;
}

int Cli::cmd_seq(int, char**, char* out, size_t len)
{
    if (!_t.seq) { return error(out, len, "seq not present"); }
    snprintf(out, len, "seq next=%lu", _t.seq->peek());
    return 0;
}

// ---------------------------------------------------------------- UART front end

int Cli::console_cb(void* ctx, int argc, char** argv)
{
    auto* self = static_cast<Cli*>(ctx);

    // esp_console already split the line; join it back so both front ends
    // go through the same execute()
    char line[MAX_LINE] = {};
    size_t pos = 0;
    for (int i = 0; i < argc; i++)
    {
        pos = appendf(line, sizeof(line), pos, "%s%s", i ? " " : "", argv[i]);
    }

    char out[MAX_REPLY];
    int rc = self->execute(line, out, sizeof(out));
    printf("%s\n", out);
    return rc;
}

void Cli::start_console()
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "pool>";
    repl_cfg.max_cmdline_length = MAX_LINE;
    repl_cfg.task_stack_size = 6144;   // commands read sensors from this task

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_t* repl = nullptr;
    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return;
    }

    static const struct { const char* name; const char* help; } COMMANDS[] = {
        { "help",   "list commands" },
        { "status", "every subsystem, one line each" },
        { "dht",    "read|status" },
        { "water",  "read|status|rom" },
        { "ph",     "read|status|temp <degC>|cal mid|low|high <v>|cal clear|cal ?" },
        { "orp",    "read|status|cal <mV>|cal clear|cal ?" },
        { "wifi",   "status|ps on|off" },
        { "mqtt",   "status" },
        { "queue",  "status|push <id> <value>" },
        { "time",   "clock and sync state" },
        { "seq",    "next sequence number" },
        { "reboot", "restart in 1 s" },
    };
    for (const auto& c : COMMANDS)
    {
        esp_console_cmd_t cmd = {};
        cmd.command = c.name;
        cmd.help = c.help;
        cmd.func_w_context = &Cli::console_cb;
        cmd.context = this;
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console ready, type help");
}
