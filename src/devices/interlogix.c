/* Interlogix/GE/UTC Wireless Device Decoder
 *
 * Copyright © 2017 Brent Bailey - bailey.brent@gmail.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "rtl_433.h"
#include "pulse_demod.h"
#include "util.h"

/*
 * Interlogix/GE/UTC Wireless 319.5 mhz Devices
 * 
 * Frequency: 319508000
 * 
 * Deocding done per us patent #5761206
 * https://www.google.com/patents/US5761206
 *
 * Protocol Bits
 * ________________________________
 * 00-02 976 uS RF front porch pulse
 * 03-14 12 sync pulses, logical zeros
 * 15 start pulse, logical one
 * 16-35 20 bit sensor identification code (ID bits 0-19)
 * 36-39 4 bit device type code (DT bits 0-3)
 * 40-42 3 bit trigger count (TC bit 0-2)
 * 43 low battery bit
 * 44 F1 latch bit NOTE: F1 latch bit and debounce are reversed.  Typo or endianess issue?
 * 45 F1 debounced level
 * 46 F2 latch bit
 * 47 F2 debounced level
 * 48 F3 latch bit (cover latch for contact sensors)
 * 49 F3 debounced level
 * 50 F4 latch bit
 * 51 F4 debounced level
 * 52 F5 positive latch bit
 * 53 F5 debounced level
 * 54 F5 negative latch bit
 * 55 even parity over odd bits 15-55
 * 56 odd parity over even bits 16-56
 * 57 zero/one, programmable
 * 58 RF on for 366 uS (old stop bit)
 * 59 one
 * 60-62 modulus 8 count of number of ones in bits 15-54
 * 63 zero (new stop bit)
 *
 * Protocol Description
 * ________________________________
 * Bits 00 to 02 are a 976 ms RF front porch pulse, providing a wake up period that allows the
 *      system controller receiver to synchronize with the incoming packet.
 * Bits 3 to 14 include 12 sync pulses, e.g., logical 0's, to synchronize the receiver.
 * Bit 15 is a start pulse, e.g., a logical 1, that tells the receiver that data is to follow.
 * Bits 16-58 provide information regarding the transmitter and associated sensor. In other
 *      embodiments, bits 16-58 may be replaced by an analog signal.
 * Bits 16 to 35 provide a 20-bit sensor identification code that uniquely identifies the particular
 *      sensor sending the message. Bits 36 to 39 provide a 4 bit device-type code that identifies the
 *      specific-type of sensor, e.g., smoke, PIR, door, window, etc. The combination of the sensor
 *      bits and device bits provide a set of data bits.
 * Bits 40 through 42 provide a 3-bit trigger count that is incremented for each group of message
 *      packets. The trigger count is a simple but effective way for preventing a third party from
 *      recording a message packet transmission and then re-transmitting that message packet
 *      transmission to make the system controller think that a valid message packet is being transmitted.
 * Bit 43 provides the low battery bit.
 * Bits 44 through 53 provide the latch bit value and the debounced value for each of the five inputs
 *      associated with the transmitter. For the F5 input, both a positive and negative latch bit are provided.
 * Bit 55 provides even parity over odd bits 15 to 55.
 * Bit 56 provides odd parity over even bits 16 to 56.
 * Bit 57 is a programmable bit that can be used for a variety of applications, including providing an
 *      additional bit that could be used for the sensor identification code or device type code.
 * Bit 58 is a 366 ms RF on signal that functions as the "old" stop bit. This bit provides compatibility with
 *      prior system controllers that may be programmed to receive a 58-bit message.
 * Bit 59 is a logical 1.
 * Bits 60 to 62 are a modulus eight count of the number of 1 bits in bits 15 through 54, providing enhanced
 *      error detection information to be used by the system controller. Finally, bit 63 is the "new" stop bit,
 *      e.g., a logical 0, that tells the system controller that it is the end of the message packet.
 *
 * Addendum
 * _______________________________
 * GE/Interlogix keyfobs do not follow the documented iti protocol and it
 *     appears the protocol was misread by the team that created the keyfobs.
 *     The button states are sent in the three trigger count bits (bit 40-42)
 *     and no battery status appears to be provided. 4 buttons and a single
 *     multi-button press (buttons 1 - lock and buttons 2 - unlock) for a total
 *     of 5 buttons available on the keyfob.
 * For contact sensors, latch 3 (typically the tamper/case open latch) will
 *     float (giving misreads) if the external contacts are used (ie; closed)
 *     and there is no 4.7 Kohm end of line resistor in place on the external
 *     circuit
 */

// message length is actually 46 but adding 2 bits for easier manipulation
#define INTERLOGIX_MSG_BIT_LEN 48

// preamble message.  only searching for 0000 0001 (bottom 8 bits of the 13 bits preamble)
static unsigned char preamble[1] = { 0x01 };

static int interlogix_callback(bitbuffer_t *bitbuffer) {
    char time_str[LOCAL_TIME_BUFLEN];
    bitrow_t *bb;
    data_t *data;
    unsigned int row = 0;
    char device_type_id[2];
    char *device_type = NULL;
    char device_serial[7];
    char device_raw_message[256];
    char *low_battery = NULL;
    char *f1_latch_state = NULL;
    char *f2_latch_state = NULL;
    char *f3_latch_state = NULL;
    char *f4_latch_state = NULL;
    char *f5_latch_state = NULL;

    // search for preamble and exit if not found
    unsigned int bit_offset = bitbuffer_search(bitbuffer, row, 0, preamble, sizeof(preamble)*8);
    if (bit_offset == bitbuffer->bits_per_row[row] || bitbuffer->num_rows != 1) {
        if (debug_output)
            fprintf(stderr, "Preamble not found, exiting! bit_offset: %d \n", bit_offset);
        return 0;
    }

    // set message starting postion (just past preamble and sync bit) and exit if msg length not met
    bit_offset += sizeof(preamble)*8;

    if (bitbuffer->bits_per_row[row] - bit_offset < INTERLOGIX_MSG_BIT_LEN - 3) {
        if (debug_output)
            fprintf(stderr, "Found valid preamble but message size (%d) too small, exiting! \n", bitbuffer->bits_per_row[row] - bit_offset);
        return 0;
    }

    uint8_t message[(INTERLOGIX_MSG_BIT_LEN/8)];

    bitbuffer_extract_bytes(bitbuffer, row, bit_offset, message, INTERLOGIX_MSG_BIT_LEN);

    // parity check: even data bits from message[0 .. 40] and odd data bits from message[1 .. 41]
    // i.e. 5 bytes and two (top-most) bits.
    int parity = message[0] ^ message[1] ^ message[2] ^ message[3] ^ message[4]; // parity as byte
    parity = (parity >> 4) ^ (parity & 0xF); // fold to nibble
    parity = (parity >> 2) ^ (parity & 0x3); // fold to 2 bits
    parity ^= message[5] >> 6; // add check bits
    int parity_error = parity ^ 0x3; // both parities are odd, i.e. 1 on success

    if (parity_error) {
        if (debug_output)
            fprintf(stderr, "Parity check failed (%d %d)\n", parity >> 1, parity & 1);
        return 0;
    }

    sprintf(device_type_id, "%01x", (reverse8(message[2])>>4));

    switch((reverse8(message[2])>>4)) {
        case 0xa: device_type = "contact"; break;
        case 0xf: device_type = "keyfob"; break;
        case 0x4: device_type = "motion"; break;
        case 0x6: device_type = "heat"; break;

        default: device_type = "unknown"; break;
    }

    sprintf(device_serial, "%02x%02x%02x", reverse8(message[2]), reverse8(message[1]), reverse8(message[0]));

    sprintf(device_raw_message, "%x%x%x", message[3], message[4], message[5]);

    // keyfob logic. see prootcol description addendum for protocol exceptions
    if ((reverse8(message[2])>>4) == 0xf) {
        low_battery = "OK";
        f1_latch_state = ((message[3] & 0xe) == 0x4) ? "CLOSED" : "OPEN";
        f2_latch_state = ((message[3] & 0xe) == 0x8) ? "CLOSED" : "OPEN";
        f3_latch_state = ((message[3] & 0xe) == 0xc) ? "CLOSED" : "OPEN";
        f4_latch_state = ((message[3] & 0xe) == 0x2) ? "CLOSED" : "OPEN";
        f5_latch_state = ((message[3] & 0xe) == 0xa) ? "CLOSED" : "OPEN";
    } else {
        low_battery = (message[3] & 0x10) ? "LOW" : "OK";
        f1_latch_state = (message[3] & 0x04) ? "OPEN" : "CLOSED";
        f2_latch_state = (message[3] & 0x01) ? "OPEN" : "CLOSED";
        f3_latch_state = (message[4] & 0x40) ? "OPEN" : "CLOSED";
        f4_latch_state = (message[4] & 0x10) ? "OPEN" : "CLOSED";
        f5_latch_state = (message[4] & 0x04) ? "OPEN" : "CLOSED";
    }

    local_time_str(0, time_str);

    data = data_make("time", "Receiver Time", DATA_STRING, time_str,
        "model", "Model", DATA_STRING, "Interlogix",
        "device_type","Device Type", DATA_STRING, device_type,
        "id","ID", DATA_STRING, device_serial,
        "device_raw_message","Raw Message", DATA_STRING, device_raw_message,
        "battery","Battery", DATA_STRING, low_battery,
        "switch1","Switch1 State", DATA_STRING, f1_latch_state,
        "switch2","Switch2 State", DATA_STRING, f2_latch_state,
        "switch3","Switch3 State", DATA_STRING, f3_latch_state,
        "switch4","Switch4 State", DATA_STRING, f4_latch_state,
        "switch5","Switch5 State", DATA_STRING, f5_latch_state,
        NULL);

    data_acquired_handler(data);

    return 1;
}

/*
 * List of fields to output when using CSV
 *
 * Used to determine what fields will be output in what
 * order for this devince when using -F csv.
 *
 */
static char *csv_output_fields[] = {
    "time",
    "model",
    "device_type",
    "id",
    "device_raw_message",
    "battery",
    "switch1",
    "switch2",
    "switch3",
    "switch4",
    "switch5",
    NULL
};

/*
 * _device - registers device/callback. see rtl_433_devices.h
 *
 * Timings:
 *
 * short, long, nad reset - specify pulse/period timings
 *     based on number of samples at 250 Khz samples/second.
 *     These timings will determine if the received pulses
 *     match, so your callback will fire after demodulation.
 *
 * for readabiliy, specify timings based on 1 Mhz samples
 *     but a divide by 4 in the definition.
 *
 *
 * Demodular:
 *
 * The function used to turn the received signal into bits.
 * See:
 * - pulse_demod.h for descriptions
 * - rtL_433.h for the list of defined names
 *
 */

r_device interlogix = {
    .name          = "Interlogix GE UTC Security Devices",
    .modulation    = OOK_PULSE_PPM_RAW,
    .short_limit   = 168, //NOTE: the nominal timing should be (122+244)/2
    .long_limit    = 1000, //Maximum gap size before new row of bits
    .reset_limit   = 500, //Maximum gap size before End Of Message
    .json_callback = &interlogix_callback,
    .disabled      = 0,
    .demod_arg     = 0,
    .fields        = csv_output_fields,
};
