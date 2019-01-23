/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos_onewire.h"

#include <string.h>
#include "mgos_gpio.h"
#include "mgos_hal.h"

#define HIGH 0x1
#define LOW 0x0

struct mgos_onewire_search_state {
  int search_mode;
  int last_device;
  int last_discrepancy;
  int last_family_discrepancy;
  uint8_t rom[8];
  uint8_t crc8;
};

struct mgos_onewire {
  int pin;
  uint8_t *res_rom;
  struct mgos_onewire_search_state sst;
};

static bool onewire_search(struct mgos_onewire *ow);

static const uint8_t crc_table[] = {
    0,   94,  188, 226, 97,  63,  221, 131, 194, 156, 126, 32,  163, 253, 31,
    65,  157, 195, 33,  127, 252, 162, 64,  30,  95,  1,   227, 189, 62,  96,
    130, 220, 35,  125, 159, 193, 66,  28,  254, 160, 225, 191, 93,  3,   128,
    222, 60,  98,  190, 224, 2,   92,  223, 129, 99,  61,  124, 34,  192, 158,
    29,  67,  161, 255, 70,  24,  250, 164, 39,  121, 155, 197, 132, 218, 56,
    102, 229, 187, 89,  7,   219, 133, 103, 57,  186, 228, 6,   88,  25,  71,
    165, 251, 120, 38,  196, 154, 101, 59,  217, 135, 4,   90,  184, 230, 167,
    249, 27,  69,  198, 152, 122, 36,  248, 166, 68,  26,  153, 199, 37,  123,
    58,  100, 134, 216, 91,  5,   231, 185, 140, 210, 48,  110, 237, 179, 81,
    15,  78,  16,  242, 172, 47,  113, 147, 205, 17,  79,  173, 243, 112, 46,
    204, 146, 211, 141, 111, 49,  178, 236, 14,  80,  175, 241, 19,  77,  206,
    144, 114, 44,  109, 51,  209, 143, 12,  82,  176, 238, 50,  108, 142, 208,
    83,  13,  239, 177, 240, 174, 76,  18,  145, 207, 45,  115, 202, 148, 118,
    40,  171, 245, 23,  73,  8,   86,  180, 234, 105, 55,  213, 139, 87,  9,
    235, 181, 54,  104, 138, 212, 149, 203, 41,  119, 244, 170, 72,  22,  233,
    183, 85,  11,  136, 214, 52,  106, 43,  117, 151, 201, 74,  20,  246, 168,
    116, 42,  200, 150, 21,  75,  169, 247, 182, 232, 10,  84,  215, 137, 107,
    53};

uint8_t mgos_onewire_crc8(const uint8_t *rom, int len) {
  uint8_t res = 0x00;
  while (len-- > 0) {
    res = crc_table[res ^ *rom++];
  }
  return res;
}

static bool onewire_wait(struct mgos_onewire *ow, uint32_t n) {
  do {
    if (n == 0) return false;
    mgos_usleep(2);
    n--;
  } while (!mgos_gpio_read(ow->pin));
  return n != 0;
}

bool mgos_onewire_reset(struct mgos_onewire *ow) {
  if (ow == NULL) return false;
  mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_INPUT);
  mgos_gpio_set_pull(ow->pin, MGOS_GPIO_PULL_UP);
  if (!onewire_wait(ow, 125)) return false;

  mgos_ints_disable();
  mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_OUTPUT);
  mgos_gpio_write(ow->pin, LOW);
  mgos_ints_enable();
  mgos_usleep(480);
  mgos_ints_disable();
  mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_INPUT);
  mgos_gpio_set_pull(ow->pin, MGOS_GPIO_PULL_UP);
  mgos_usleep(70);
  bool res = !mgos_gpio_read(ow->pin);
  mgos_ints_enable();
  mgos_usleep(410);
  return res;
}

/*
 * Setup the search to find the device type 'family_code'
 * on the next call mgos_onewire_next() if it is present
 * Note if no devices of the desired family are currently
 * on the 1-Wire, then another type will be found.
 */
void mgos_onewire_target_setup(struct mgos_onewire *ow, uint8_t family_code) {
  if (ow == NULL) return;
  memset(&ow->sst, 0, sizeof(ow->sst));
  ow->sst.rom[0] = family_code;
  ow->sst.last_discrepancy = 64;
}

/*
 * Mode:
 * 0 - normal search, 1 - conditional search
 */
bool mgos_onewire_next(struct mgos_onewire *ow, uint8_t *rom, int mode) {
  if (ow == NULL) return 0;
  ow->res_rom = rom;
  ow->sst.search_mode = mode;
  return onewire_search(ow);
}

void mgos_onewire_select(struct mgos_onewire *ow, const uint8_t *rom) {
  if (ow == NULL) return;
  mgos_onewire_write(ow, 0x55);
  for (int i = 0; i < 8; i++) {
    mgos_onewire_write(ow, rom[i]);
  }
}

void mgos_onewire_skip(struct mgos_onewire *ow) {
  if (ow == NULL) return;
  mgos_onewire_write(ow, 0xCC);
}

void mgos_onewire_search_clean(struct mgos_onewire *ow) {
  if (ow == NULL) return;
  memset(&ow->sst, 0, sizeof(ow->sst));
}

static bool onewire_search(struct mgos_onewire *ow) {
  uint8_t id_bit_number = 1, last_zero = 0, rom_byte_number = 0, id_bit,
          cmp_id_bit, rom_byte_mask = 1, dir;
  bool res = false;
  if (!ow->sst.last_device) {
    if (!mgos_onewire_reset(ow)) {
      ow->sst.last_device = 0;
      ow->sst.last_discrepancy = 0;
      ow->sst.last_family_discrepancy = 0;
      return 0;
    }

    if (!ow->sst.search_mode) {
      /* normal search */
      mgos_onewire_write(ow, 0xF0);
    } else {
      /* conditional search */
      mgos_onewire_write(ow, 0xEC);
    }

    do {
      id_bit = mgos_onewire_read_bit(ow);
      cmp_id_bit = mgos_onewire_read_bit(ow);

      if ((id_bit == 1) && (cmp_id_bit == 1)) {
        break;
      } else {
        if (id_bit != cmp_id_bit) {
          dir = id_bit;
        } else {
          if (id_bit_number < ow->sst.last_discrepancy) {
            dir = ((ow->sst.rom[rom_byte_number] & rom_byte_mask) > 0);
          } else {
            dir = (id_bit_number == ow->sst.last_discrepancy);
          }
          if (dir == 0) {
            last_zero = id_bit_number;
            if (last_zero < 9) {
              ow->sst.last_family_discrepancy = last_zero;
            }
          }
        }

        if (dir == 1) {
          ow->sst.rom[rom_byte_number] |= rom_byte_mask;
        } else {
          ow->sst.rom[rom_byte_number] &= ~rom_byte_mask;
        }

        mgos_onewire_write_bit(ow, dir);

        id_bit_number++;
        rom_byte_mask <<= 1;

        if (rom_byte_mask == 0) {
          rom_byte_number++;
          rom_byte_mask = 1;
        }
      }
    } while (rom_byte_number < 8);

    if (!(id_bit_number < 65)) {
      ow->sst.last_discrepancy = last_zero;
      if (ow->sst.last_discrepancy == 0) {
        ow->sst.last_device = 1;
      }
      res = true;
    }
  }

  if (res && ow->sst.rom[0] &&
      (ow->sst.rom[7] == mgos_onewire_crc8(ow->sst.rom, 7))) {
    for (int i = 0; i < 8; i++) ow->res_rom[i] = ow->sst.rom[i];
  } else {
    res = false;
    ow->sst.last_device = 0;
    ow->sst.last_discrepancy = 0;
    ow->sst.last_family_discrepancy = 0;
  }
  return res;
}

bool mgos_onewire_read_bit(struct mgos_onewire *ow) {
  bool res;

  if (ow == NULL) return 0;
  // mgos_ints_disable();

  // Issue: if any of the code executed in this critical section is not cache-resident
  // and the bit being received is a '0', the execution delays involved in caching the
  // code may cause the '0' bit to be interpreted as a '1'.
  //
  // Fix: We ensure that the critical code is cached by executing it twice: once to make sure it is cached,
  // and a second time to actually perform the time-critical read operation.
  //
  // The first time through:
  //   - the Onewire data pin is driven to '1' instead of '0' to avoid starting the Onewire READ operation
  //   - the loop timing delays are set to minimal values since they are not needed first time through
  // The second time through, the code is known to be cache-resident.  Therefore:
  //   - The OneWire data pin gets driven to '0' which starts the time-critical READ operation
  //   - timing delays are executed with their proper values

  const int delay_param_F = 52;     // Min 50, Typ. 55, Max <none>

  // Init params to have little or no effect the first time through:
  int delay_param_A = 1;
  int delay_param_E = 1;
  int data_state = 1;

  int iterations = 2;

  do {
    mgos_gpio_write(ow->pin, data_state);                 // To avoid potential glitches, set the new output state...
    mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_OUTPUT);   // ...before changing the mode
    mgos_usleep(delay_param_A);
    mgos_gpio_set_pull(ow->pin, MGOS_GPIO_PULL_UP);       // To avoid potential glitches, enable pullups...
    mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_INPUT);    // ...before changing the mode
    mgos_usleep(delay_param_E);
    res = mgos_gpio_read(ow->pin);

    // Prepare for the actual read operation:
    data_state = 0;
    delay_param_A = 6;      // Min 5, Typ. 6, Max 15    Test system started producing errors at < 5 
    delay_param_E = 9;      // Min 5, Typ. 9, Max 12    Test system started producing errors at > 17
  } while (--iterations);

  // mgos_ints_enable();
  mgos_usleep(delay_param_F);
  return res;
}

uint8_t mgos_onewire_read(struct mgos_onewire *ow) {
  uint8_t res = 0x00;
  mgos_ints_disable();
  for (uint8_t mask = 0x01; mask; mask <<= 1) {
    if (mgos_onewire_read_bit(ow)) res |= mask;
  }
  mgos_ints_enable();
  return res;
}

void mgos_onewire_read_bytes(struct mgos_onewire *ow, uint8_t *buf, int len) {
  for (int i = 0; i < len; i++) {
    buf[i] = mgos_onewire_read(ow);
  }
}

void mgos_onewire_write_bit(struct mgos_onewire *ow, int bit) {
  if (ow == NULL) return;
  mgos_ints_disable();
  mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_OUTPUT);
  mgos_gpio_write(ow->pin, LOW);
  mgos_usleep(bit ? 10 : 65);
  mgos_gpio_write(ow->pin, HIGH);
  mgos_ints_enable();
  mgos_usleep(bit ? 55 : 5);
}

void mgos_onewire_write(struct mgos_onewire *ow, const uint8_t data) {
  for (uint8_t mask = 0x01; mask; mask <<= 1) {
    mgos_onewire_write_bit(ow, (mask & data) ? 1 : 0);
  }
}

void mgos_onewire_write_bytes(struct mgos_onewire *ow, const uint8_t *buf,
                              int len) {
  for (int i = 0; i < len; i++) {
    mgos_onewire_write(ow, buf[i]);
  }
}

struct mgos_onewire *mgos_onewire_create(int pin) {
  struct mgos_onewire *ow = calloc(1, sizeof(*ow));
  if (ow == NULL) return NULL;
  ow->pin = pin;
  if (!mgos_gpio_set_mode(ow->pin, MGOS_GPIO_MODE_INPUT) ||
      !mgos_gpio_set_pull(ow->pin, MGOS_GPIO_PULL_UP)) {
    mgos_onewire_close(ow);
    return NULL;
  }
  return ow;
}

void mgos_onewire_close(struct mgos_onewire *ow) {
  free(ow);
}

bool mgos_onewire_init(void) {
  return true;
}
