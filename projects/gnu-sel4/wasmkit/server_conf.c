/*
 * posix_server's configuration on wasmkit (TODO item 24, 1b): the sDDF structures Microkit's
 * tool would patch into its ELF (meta.py's serial and timer clients), filled before init.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <sddf/serial/config.h>
#include <sddf/timer/config.h>
#include "layout.h"

extern serial_client_config_t serial_config;
extern timer_client_config_t timer_config;

void wasmkit_configure(void)
{
    __builtin_memcpy(serial_config.magic, SDDF_SERIAL_MAGIC, SDDF_SERIAL_MAGIC_LEN);
    serial_config.rx = (serial_connection_resource_t){
        { (void *)WK_SERIAL_RX_QUEUE, 0x1000 }, { (void *)WK_SERIAL_RX_DATA, WK_SERIAL_RX_SIZE }, WK_SERVER_SERIAL_RX };
    serial_config.tx = (serial_connection_resource_t){
        { (void *)WK_SERIAL_TX_QUEUE, 0x1000 }, { (void *)WK_SERIAL_TX_DATA, WK_SERIAL_TX_SIZE }, WK_SERVER_SERIAL_TX };
    __builtin_memcpy(timer_config.magic, SDDF_TIMER_MAGIC, SDDF_TIMER_MAGIC_LEN);
    timer_config.driver_id = WK_SERVER_TIMER;
}
