#ifndef CLI_CLASS_H
#define CLI_CLASS_H

#include <stddef.h>

#include "dht_sensor.h"
#include "ds18b20.h"
#include "ezo_sensor.h"
#include "wifi.h"
#include "mqtt.h"
#include "seq_counter.h"
#include "telemetry.h"

/**
 * @brief Text command interface for bench testing and server commands (POOL-6).
 *
 * One dispatcher, two front ends: the UART console (esp_console REPL) and the
 * MQTT cmd topic. Both hand a line to execute() and get a text reply back, so
 * a command typed at the bench is exactly what the server would send.
 *
 * Commands are "<object> <action> [args]", e.g. "ph cal mid 7.00". Send "help"
 * for the list. Any object whose pointer is null answers "not present".
 */
class Cli
{
public:
    /**
     * @brief Everything the commands can reach. Any pointer may be null.
     */
    struct Targets
    {
        DhtSensor*    dht;
        Ds18b20*      water;
        EzoSensor*    ph;
        EzoSensor*    orp;
        Wifi*         wifi;
        MqttClient*   mqtt;
        ReadingQueue* queue;
        SeqCounter*   seq;
    };

    constexpr static size_t MAX_LINE  = 128;    ///< longest command line accepted
    constexpr static size_t MAX_REPLY = 512;    ///< reply buffer callers should provide

private:
    Targets _t;

    constexpr static const char* TAG = "[CLI]";

    // One handler per object; each writes its reply into out and returns 0 on success
    int cmd_help  (int argc, char** argv, char* out, size_t len);
    int cmd_status(int argc, char** argv, char* out, size_t len);
    int cmd_dht   (int argc, char** argv, char* out, size_t len);
    int cmd_water (int argc, char** argv, char* out, size_t len);
    int cmd_ezo   (EzoSensor* s, const char* name, int argc, char** argv, char* out, size_t len);
    int cmd_wifi  (int argc, char** argv, char* out, size_t len);
    int cmd_mqtt  (int argc, char** argv, char* out, size_t len);
    int cmd_queue (int argc, char** argv, char* out, size_t len);
    int cmd_time  (int argc, char** argv, char* out, size_t len);
    int cmd_seq   (int argc, char** argv, char* out, size_t len);

    static int console_cb(void* ctx, int argc, char** argv);

public:
    explicit Cli(const Targets& targets);

    /**
     * @brief Run one command line and write the reply.
     *
     * Safe to call from any task; sensor access is serialised with the
     * samplers. Blocks for as long as the command takes (up to ~1 s for an
     * EZO read).
     *
     * @param line Command line, e.g. "water read"
     * @param out Receives the reply text, always NUL-terminated
     * @param len Size of out; MAX_REPLY fits every reply
     * @return 0 on success, 1 on a usage or runtime error (the reply says which)
     */
    int execute(const char* line, char* out, size_t len);

    /**
     * @brief Start the UART console REPL with every command registered.
     *
     * Call once after the targets exist. The REPL runs in its own task.
     */
    void start_console();

    Cli(const Cli&) = delete;
    Cli& operator=(const Cli&) = delete;
};

#endif // CLI_CLASS_H
