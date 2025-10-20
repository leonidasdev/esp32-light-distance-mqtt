#ifndef COMPONENTS_TELEGRAM_MANAGER_INCLUDE_TELEGRAM_H_
#define COMPONENTS_TELEGRAM_MANAGER_INCLUDE_TELEGRAM_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize Telegram module from a token file located on the data partition.
 * The expected `token_file_path` format (lines):
 *  - line 1: bot token (e.g. 1234:ABC...)
 *  - line 2: optional admin chat id or comment
 *  - line 3: persisted last_update_id (optional, integer) to avoid replaying
 *
 * Returns true on success (file found and parsed), false on failure.
 */
bool telegram_init_from_file(const char *token_file_path);

/** Start the Telegram long-poll task. Must be called after networking is up. */
void telegram_start(void);

/**
 * Register a message handler called for each incoming update.
 * Handler signature: (chat_id, text, user_ctx)
 */
void telegram_register_message_handler(void (*handler)(int64_t, const char *, void *), void *user_ctx);

/** Blocking send of a text message to `chat_id`. Returns true on success. */
bool telegram_send_message(int64_t chat_id, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* COMPONENTS_TELEGRAM_MANAGER_INCLUDE_TELEGRAM_H_ */
