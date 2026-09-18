#pragma once

#include <stddef.h>
#include <stdint.h>

/* RevEmu 2013 connect blob. Reunion classifies it as CA_REVEMU2013.
 * Layout matches CRevEmu2013Authorizer in rehlds/ReUnion. */
#define REVEMU2013_TICKET_SIZE 194
#define REVEMU2013_KEY_LEN     32

uint32_t RevEmu2013_Hash(const char *str);
uint32_t RevEmu2013_AccountId(const char *str);
int RevEmu2013_WriteTicket(void *blob, int maxBytes, const char *auth_key);
int RevEmu2013_AccountIdFromTicket(const void *blob, int len, uint32_t *account_id);
