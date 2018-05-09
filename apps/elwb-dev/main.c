/*
 * Copyright (c) 2018, Swiss Federal Institute of Technology (ETH Zurich).
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
 *          Tonio Gsell
 */

/**
 * @brief eLWB Development Application (ETZ Test deployment)
 */

#include "main.h"

/*---------------------------------------------------------------------------*/
#ifdef APP_TASK_ACT_PIN
 #define APP_TASK_ACTIVE    PIN_SET(APP_TASK_ACT_PIN)
 #define APP_TASK_INACTIVE  PIN_CLR(APP_TASK_ACT_PIN)
#else 
 #define APP_TASK_ACTIVE
 #define APP_TASK_INACTIVE
#endif /* APP_TASK_ACT_PIN */
/*---------------------------------------------------------------------------*/
/* global variables */
rtimer_clock_t bolt_captured_trq = 0;     /* last captured timestamp */
uint16_t seq_no_lwb  = 0;
uint16_t seq_no_bolt = 0;
config_t cfg;
static uint32_t last_health_pkt = 0;
static uint16_t max_stack_size = 0;
//static uint8_t  rf_tx_pwr = RF_CONF_TX_POWER;
/*---------------------------------------------------------------------------*/
void
capture_timestamp(void)
{ 
  /* simply store the timestamp, do calculations afterwards */
  rtimer_clock_t now = rtimer_now_lf();
  bolt_captured_trq = now - ((uint16_t)(now & 0xffff) - BOLT_CONF_TIMEREQ_CCR);
}
/*---------------------------------------------------------------------------*/
void
update_time(void)
{
  if(utc_time_updated) {
    /* a UTC timestamp has been received -> update the network time */
    uint32_t elapsed = 0;
    /* adjust the timestamp to align with the next communication round 
    * as closely as possible */
    rtimer_clock_t next_round;
    if(rtimer_next_expiration(LWB_CONF_LF_RTIMER_ID, &next_round)) {
      elapsed = (next_round - utc_time_rx);          
    } else {
      DEBUG_PRINT_WARNING("invalid timer value");
    }
    /* convert to us */
    elapsed = elapsed * (1000000 / RTIMER_SECOND_LF);
    uint32_t new_time = (utc_time + elapsed) / 1000000;
    uint32_t curr_time = lwb_get_time(0);
    /* only update if the difference is much larger than 1 second */
    uint16_t diff;
    curr_time += lwb_sched_get_period();
    if(new_time > curr_time) {
      diff = new_time - curr_time;
    } else {
      diff = curr_time - new_time;
    }
    if(diff > UTC_TIMESTAMP_MAX_DRIFT) {
      lwb_sched_set_time(new_time);
      DEBUG_PRINT_INFO("timestamp adjusted to %lu", new_time);
      EVENT_INFO(EVENT_CC430_TIME_UPDATED, diff);
    } else {
      DEBUG_PRINT_INFO("timestamp: %lu, drift: %u", new_time, diff);
    }
    utc_time_updated = 0;
  }
}
/*---------------------------------------------------------------------------*/
PROCESS(app_proc_pre, "App Task Pre");
PROCESS_THREAD(app_proc_pre, ev, data) 
{  
  PROCESS_BEGIN();
  
  while(1) {  
    APP_TASK_INACTIVE;
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    APP_TASK_ACTIVE;
    
    AFTER_DEEPSLEEP();    /* restore all clocks */
    
    /* --- read messages from the BOLT queue and forward them to the LWB --- */
    uint8_t  bolt_buffer[BOLT_CONF_MAX_MSG_LEN];
    uint16_t msg_cnt = 0;
    while(BOLT_DATA_AVAILABLE &&
          (lwb_get_send_buffer_state() < LWB_CONF_OUT_BUFFER_SIZE)) {
      if(bolt_read(bolt_buffer)) {
        msg_cnt++;
        process_message((dpp_message_t*)bolt_buffer, 1);
      } /* else: invalid message received from BOLT */
    }
    if(msg_cnt) {
      DEBUG_PRINT_INFO("%u message(s) read from BOLT", msg_cnt);
    }
    
  #if (NODE_ID == HOST_ID) && TIMESYNC_HOST_RCV_UTC
    update_time();
  #endif
  }
  
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS(app_proc_post, "App Task Post");
AUTOSTART_PROCESSES(&app_proc_post);
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(app_proc_post, ev, data) 
{
  PROCESS_BEGIN();

  /* --- initialization --- */

  static uint8_t node_info_sent = 0;
  
  /* init the ADC */
  adc_init();
  REFCTL0 &= ~REFON;             /* shut down REF module to save power */

  /* start the preprocess and LWB threads */
  lwb_start(&app_proc_pre, &app_proc_post);
  process_start(&app_proc_pre, NULL);
  
  /* --- load the configuration --- */
  if(!nvcfg_load((uint8_t*)&cfg)) {
    DEBUG_PRINT_MSG_NOW("WARNING: failed to load config");
  }
  /* update stats and save */
  cfg.rst_cnt++;
  nvcfg_save((uint8_t*)&cfg);
  
  /* enable the timestamp request interrupt */
  bolt_set_timereq_callback(capture_timestamp);

  /* --- start of application main loop --- */
  while(1) {
    /* the app task should not do anything until it is explicitly granted 
     * permission (by receiving a poll event) by the LWB task */
    APP_TASK_INACTIVE;
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    APP_TASK_ACTIVE;
        
    dpp_message_t msg;
    uint16_t msg_cnt = 0;

    /* --- process all packets rcvd from the network (forward to BOLT) --- */
    while(lwb_rcv_pkt((uint8_t*)&msg, 0, 0)) {
      process_message(&msg, 0);
      msg_cnt++;
    }
    if(msg_cnt) {
      DEBUG_PRINT_INFO("%d msg rcvd from network", msg_cnt);
    }
    
  #if (NODE_ID != HOST_ID) || !TIMESYNC_HOST_RCV_UTC
    /* --- send the timestamp if one has been requested --- */
    if(bolt_captured_trq) {
      send_timestamp(bolt_captured_trq);
      bolt_captured_trq = 0;
    }
  #endif /* !TIMESYNC_HOST_RCV_UTC */
  
    /* adjust the TX power */
    /*uint16_t snr = lwb_get_stats()->glossy_snr;
    if(snr > 0) {   // SNR valid?
      if(snr > 50 && rf_tx_pwr > 0) {
        rf_tx_pwr--;
        rf1a_set_tx_power(rf_tx_pwr);
        DEBUG_PRINT_INFO("TX power reduced");
      } else if (snr < 20 && rf_tx_pwr < RF1A_TX_POWER_MAX) {
        rf_tx_pwr++;
        rf1a_set_tx_power(rf_tx_pwr);
        DEBUG_PRINT_INFO("TX power increased");
      }
    }*/
    
    /* generate a node info message if necessary (must be here) */
    if(!node_info_sent) {
  #if (NODE_ID != HOST_ID)
      if(lwb_get_time(0)) { 
  #else
      if(!TIMESYNC_HOST_RCV_UTC || utc_time) {
  #endif
        send_node_info();
        node_info_sent = 1;
      }
    } else {
      /* only send other messages once the node info msg has been sent! */
    
      /* --- generate a new health message if necessary --- */
      uint16_t div = lwb_get_time(0) / health_msg_period;
      if(div != last_health_pkt) {
        /* using a divider instead of the elapsed time will group the health
        * messages of all nodes together into one round */
        send_node_health();
        send_lwb_health();
        last_health_pkt = div;
      }
    }
    
    /* --- debugging --- */
    uint16_t stack_size = debug_print_get_max_stack_size();
    if(stack_size > max_stack_size) {
      max_stack_size = stack_size;
      DEBUG_PRINT_INFO("stack size: %uB, max %uB", (SRAM_START + SRAM_SIZE) -
                                            (uint16_t)&stack_size, stack_size);
    }
    
    /* --- poll the debug task --- */
    debug_print_poll();
    process_poll(&app_proc_post);
    APP_TASK_INACTIVE;
    PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);
    APP_TASK_ACTIVE;
    
    /* since this process is only executed at the end of an LWB round, we 
     * can now configure the MCU for minimal power dissipation for the idle
     * period until the next round starts */
    BEFORE_DEEPSLEEP();
  } /* --- end of application main loop --- */

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
