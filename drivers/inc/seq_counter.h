#ifndef SEQ_COUNTER_H
#define SEQ_COUNTER_H

#include <stdint.h>         // for uint32_t
#include "nvs.h"            // for nvs_handle_t

/**
 * @brief Telemetry sequence number that survives reboot and is never reused (POOL-38).
 *
 * seq is how the server detects gaps without asking, and it is the key for
 * buffered replay later (POOL-3). A seq that restarts at 0 after a reboot
 * would look like duplicates, so every value is persisted to NVS before use.
 *
 * At one write per published reading (a handful per half hour) NVS wear
 * levelling makes flash endurance a non-issue.
 */
class SeqCounter
{
public:
    enum class ReturnCode {
        OK,
        ALREADY_INITIALIZED,
        INTERNAL_ERROR          ///< NVS open/read failed; counting continues in RAM only
    };

private:
    nvs_handle_t _nvs;          ///< Handle to the NVS namespace holding the counter
    uint32_t _next;             ///< Next value to hand out
    bool _persist;              ///< false if NVS is unavailable

    constexpr static const char* TAG = "[SEQ]";

public:
    SeqCounter();

    /**
     * @brief Open NVS and resume from the last persisted value.
     *
     * nvs_flash_init() must have run first.
     *
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode init();

    /**
     * @brief Hand out the next sequence number.
     *
     * The value after it is written to NVS before returning, so a crash right
     * after publishing can never cause this value to be reused.
     *
     * @return The sequence number to put in the message
     */
    uint32_t next();

    /**
     * @brief The value next() will hand out, without consuming it.
     */
    uint32_t peek() const { return _next; }

    SeqCounter(const SeqCounter&) = delete;
    SeqCounter& operator=(const SeqCounter&) = delete;
};

#endif // SEQ_COUNTER_H
