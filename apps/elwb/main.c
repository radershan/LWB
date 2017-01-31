/*
 * Copyright (c) 2016, Swiss Federal Institute of Technology (ETH Zurich).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author:  Reto Da Forno
 *          Felix Sutton
 */
 
/*
 * eLWB app, runs on the DPP
 */

#include "main.h"

/*---------------------------------------------------------------------------*/
#ifdef APP_TASK_ACT_PIN
#define TASK_ACTIVE             PIN_SET(APP_TASK_ACT_PIN)
#define TASK_SUSPENDED          PIN_CLR(APP_TASK_ACT_PIN)
#else  /* APP_TASK_ACT_PIN */
#define TASK_ACTIVE
#define TASK_SUSPENDED
#endif /* APP_TASK_ACT_PIN */
/*---------------------------------------------------------------------------*/
static message_t msg_buffer;
/*---------------------------------------------------------------------------*/
void
bolt_read_msg(void)
{
  /* uint16_t to avoid aliasing issues */
  static uint16_t bolt_buffer[(BOLT_CONF_MAX_MSG_LEN + 1) / 2];
  uint16_t max_ops = 10;
  while(BOLT_DATA_AVAILABLE && max_ops) {
    uint8_t msg_len = 0;
    BOLT_READ((uint8_t*)bolt_buffer, msg_len);
    if(msg_len) {
      DEBUG_PRINT_INFO("message read from Bolt (%db)", msg_len);
      /* correct the message length (cut excess characters) */
      msg_len = MSG_LEN_PTR((message_t*)bolt_buffer);
      if(msg_len > 2 && msg_len <= BOLT_CONF_MAX_MSG_LEN) {
        /* check the CRC */
        if(MSG_GET_CRC16((message_t*)bolt_buffer) != 
          crc16((uint8_t*)bolt_buffer, msg_len - 2, 0)) {
          DEBUG_PRINT_WARNING("invalid CRC (dropped)");
        } else {
          lwb_send_pkt(0, 1, (uint8_t*)bolt_buffer, msg_len);
        }
      } else {
        DEBUG_PRINT_WARNING("invalid msg length (dropped)");
      }
    }
    max_ops--;
  }
}
/*---------------------------------------------------------------------------*/
void
host_run(void)
{
  /* print out the received data */
  uint8_t pkt_len;
  do {
    pkt_len = lwb_rcv_pkt((uint8_t*)&msg_buffer, 0, 0);
    if(pkt_len) {
      DEBUG_PRINT_INFO("msg (%u bytes) rcvd from node %u", 
                       pkt_len, 
                       msg_buffer.header.device_id);
      /* verify CRC */
      if(MSG_GET_CRC16(&msg_buffer) != 
         crc16((uint8_t*)&msg_buffer, pkt_len - 2, 0)) {
        DEBUG_PRINT_WARNING("invalid CRC (dropped)");
      } else {
        if(msg_buffer.header.type == MSG_TYPE_AE_EVENT) {
          DEBUG_PRINT_INFO("AE event received (ID: %u, timestamp: %llu)",
                           msg_buffer.ae_evt.event_id, 
                           msg_buffer.ae_evt.generation_time); 
        }
        /* forward the packet to the app processor */
        BOLT_WRITE((uint8_t*)&msg_buffer, pkt_len);
      }
    }
  } while(pkt_len);
  
#if !TIMESYNC_INTERRUPT_BASED
  rtimer_clock_t bolt_timestamp = 0;
  if(bolt_handle_timereq(&bolt_timestamp)) { 
    msg_buffer.header.device_id   = node_id;
    msg_buffer.header.type        = MSG_TYPE_TIMESYNC;
    msg_buffer.header.payload_len = sizeof(timestamp_t);
    msg_buffer.timestamp          = bolt_timestamp;
    BOLT_WRITE((uint8_t*)&msg_buffer, MSG_LEN(msg_buffer));
  }
#endif /* TIMESYNC_INTERRUPT_BASED */
}
/*---------------------------------------------------------------------------*/
void
source_run(void)
{
  if(lwb_rcv_pkt((uint8_t*)&msg_buffer, 0, 0)) {
    DEBUG_PRINT_INFO("pkt received");
  }
    
#if !TIMESYNC_INTERRUPT_BASED
  rtimer_clock_t bolt_timestamp = 0;
  if(bolt_handle_timereq(&bolt_timestamp)) {
    rtimer_clock_t local_rx_time = 0;
    uint32_t lwb_time_secs = lwb_get_time(&local_rx_time);
    int64_t lwb_time   = (uint64_t)lwb_time_secs * RTIMER_SECOND_HF;
    int64_t ofs        = lwb_time - local_rx_time;
    bolt_timestamp     = bolt_timestamp + ofs + TIMESYNC_OFS;
    msg_buffer.header.device_id   = node_id;
    msg_buffer.header.type        = MSG_TYPE_TIMESYNC;
    msg_buffer.header.payload_len = sizeof(timestamp_t);
    msg_buffer.timestamp          = bolt_timestamp;
    BOLT_WRITE((uint8_t*)&msg_buffer, MSG_LEN(msg_buffer));
  }
#endif /* TIMESYNC_INTERRUPT_BASED */
}
/*---------------------------------------------------------------------------*/
#if TIMESYNC_INTERRUPT_BASED
void
bolt_timereq_cb(void)
{
  /* timestamp request: calculate the timestamp and send it over BOLT */
  rtimer_clock_t now            = rtimer_now_hf();
  if(node_id == HOST_ID) {
    msg_buffer.timestamp        = now - ((uint16_t)(now & 0xffff) -
                                  BOLT_CONF_TIMEREQ_CCR);
  } else {
    rtimer_clock_t captured     = now - ((uint16_t)(now & 0xffff) -
                                  BOLT_CONF_TIMEREQ_CCR);
    /* convert the local timestamp into global (LWB) time */
    rtimer_clock_t local_t_rx   = 0;
    uint32_t lwb_time_secs      = lwb_get_time(&local_t_rx);
    int64_t  lwb_time           = (uint64_t)lwb_time_secs * RTIMER_SECOND_HF;
    int64_t  ofs                = lwb_time - local_t_rx;
    msg_buffer.timestamp        = captured + ofs + TIMESYNC_OFS;
    /* drift compensation */
    const lwb_statistics_t* lwb_stats = lwb_get_stats();
    if(lwb_stats->drift) {
      int32_t elapsed           = (int64_t)captured - local_t_rx;
      int16_t drift_comp        = (int16_t)(elapsed *
                                  (int32_t)lwb_stats->drift /
                                  (int32_t)RTIMER_SECOND_HF); 
      msg_buffer.timestamp     -= drift_comp;
      /* debug only */
      //DEBUG_PRINT_INFO("drift compensation: %d", drift_comp);
    }
  }
  msg_buffer.header.device_id   = node_id;
  msg_buffer.header.type        = MSG_TYPE_TIMESYNC;
  msg_buffer.header.payload_len = sizeof(timestamp_t);
  BOLT_WRITE((uint8_t*)&msg_buffer, MSG_LEN(msg_buffer));
}
#endif /* TIMESYNC_INTERRUPT_BASED */
/*---------------------------------------------------------------------------*/
PROCESS(app_process, "Application Task");
AUTOSTART_PROCESSES(&app_process);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(app_process, ev, data) 
{  
  PROCESS_BEGIN();
  
  /* error checks */
  if(sizeof(message_t) != MSG_PKT_LEN) {
    DEBUG_PRINT_MSG_NOW("ERROR: message_t is too long");
  }
  /* start the LWB thread with a pre and post processing task */
  lwb_start(bolt_read_msg, &app_process);

#if TIMESYNC_INTERRUPT_BASED
  bolt_set_timereq_callback(bolt_timereq_cb);
#endif /* TIMESYNC_INTERRUPT_BASED */
  
  /* main loop of the application task */
  while(1) {
    /* the app task should not do anything until it is explicitly granted 
     * permission (by receiving a poll event) by the LWB task */
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    TASK_ACTIVE;      /* application task runs now */
        
    if(HOST_ID == node_id) {
      host_run();
    } else {
      source_run();
    }

  #if LWB_CONF_USE_LF_FOR_WAKEUP
    LWB_BEFORE_DEEPSLEEP();     /* prepare to go to LPM3 */
  #endif /* LWB_CONF_USE_LF_FOR_WAKEUP */
    TASK_SUSPENDED;
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
