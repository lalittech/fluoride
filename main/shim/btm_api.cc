/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "bt_shim_btm"

#include <base/callback.h>

#include <mutex>

#include "common/time_util.h"
#include "device/include/controller.h"
#include "gd/common/callback.h"
#include "gd/neighbor/name.h"
#include "main/shim/btm.h"
#include "main/shim/btm_api.h"
#include "main/shim/controller.h"
#include "main/shim/helpers.h"
#include "main/shim/shim.h"
#include "main/shim/stack.h"
#include "stack/btm/btm_int_types.h"
#include "types/raw_address.h"

/**
 * Legacy bluetooth module global control block state
 *
 * Mutex is used to synchronize access from the shim
 * layer into the global control block.  This is used
 * by the shim despite potentially arbitrary
 * unsynchronized access by the legacy stack.
 */
extern tBTM_CB btm_cb;
std::mutex btm_cb_mutex_;

extern bool btm_inq_find_bdaddr(const RawAddress& p_bda);
extern tINQ_DB_ENT* btm_inq_db_find(const RawAddress& raw_address);
extern tINQ_DB_ENT* btm_inq_db_new(const RawAddress& p_bda);

/**
 * Legacy bluetooth btm stack entry points
 */
extern void btm_acl_update_busy_level(tBTM_BLI_EVENT event);
extern void btm_clear_all_pending_le_entry(void);
extern void btm_clr_inq_result_flt(void);
extern void btm_set_eir_uuid(uint8_t* p_eir, tBTM_INQ_RESULTS* p_results);
extern void btm_sort_inq_result(void);
extern void btm_process_inq_complete(uint8_t status, uint8_t result_type);

static bool max_responses_reached() {
  return (btm_cb.btm_inq_vars.inqparms.max_resps &&
          btm_cb.btm_inq_vars.inq_cmpl_info.num_resp >=
              btm_cb.btm_inq_vars.inqparms.max_resps);
}

static bool is_periodic_inquiry_active() {
  return btm_cb.btm_inq_vars.inq_active & BTM_PERIODIC_INQUIRY_ACTIVE;
}

static bool has_le_device(tBT_DEVICE_TYPE device_type) {
  return device_type & BT_DEVICE_TYPE_BLE;
}

static bool is_classic_device(tBT_DEVICE_TYPE device_type) {
  return device_type == BT_DEVICE_TYPE_BREDR;
}

static bool has_classic_device(tBT_DEVICE_TYPE device_type) {
  return device_type & BT_DEVICE_TYPE_BREDR;
}

static bool is_dual_mode_device(tBT_DEVICE_TYPE device_type) {
  return device_type == BT_DEVICE_TYPE_DUMO;
}

static bool is_observing_or_active_scanning() {
  return btm_cb.btm_inq_vars.inqparms.mode & BTM_BLE_INQUIRY_MASK;
}

static void check_exceeded_responses(tBT_DEVICE_TYPE device_type,
                                     bool scan_rsp) {
  if (!is_periodic_inquiry_active() && max_responses_reached() &&
      ((is_observing_or_active_scanning() && is_dual_mode_device(device_type) &&
        scan_rsp) ||
       (is_observing_or_active_scanning()))) {
    LOG_INFO("UNIMPLEMENTED %s Device max responses found...cancelling inquiry",
             __func__);
  }
}

void btm_api_process_inquiry_result(const RawAddress& raw_address,
                                    uint8_t page_scan_rep_mode,
                                    DEV_CLASS device_class,
                                    uint16_t clock_offset) {
  tINQ_DB_ENT* p_i = btm_inq_db_find(raw_address);
  if (max_responses_reached()) {
    if (p_i == nullptr || !has_le_device(p_i->inq_info.results.device_type)) {
      return;
    }
  }

  if (p_i == nullptr) {
    p_i = btm_inq_db_new(raw_address);
    CHECK(p_i != nullptr);
  } else if (p_i->inq_count == btm_cb.btm_inq_vars.inq_counter &&
             is_classic_device(p_i->inq_info.results.device_type)) {
    return;
  }

  p_i->inq_info.results.page_scan_rep_mode = page_scan_rep_mode;
  p_i->inq_info.results.page_scan_per_mode = 0;  // RESERVED
  p_i->inq_info.results.page_scan_mode = 0;      // RESERVED
  p_i->inq_info.results.dev_class[0] = device_class[0];
  p_i->inq_info.results.dev_class[1] = device_class[1];
  p_i->inq_info.results.dev_class[2] = device_class[2];
  p_i->inq_info.results.clock_offset = clock_offset | BTM_CLOCK_OFFSET_VALID;
  p_i->inq_info.results.inq_result_type = BTM_INQ_RESULT_BR;
  p_i->inq_info.results.rssi = BTM_INQ_RES_IGNORE_RSSI;

  p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
  p_i->inq_count = btm_cb.btm_inq_vars.inq_counter;
  p_i->inq_info.appl_knows_rem_name = false;

  if (p_i->inq_count != btm_cb.btm_inq_vars.inq_counter) {
    p_i->inq_info.results.device_type = BT_DEVICE_TYPE_BREDR;
    btm_cb.btm_inq_vars.inq_cmpl_info.num_resp++;
    p_i->scan_rsp = false;
  } else {
    p_i->inq_info.results.device_type |= BT_DEVICE_TYPE_BREDR;
  }

  check_exceeded_responses(p_i->inq_info.results.device_type, p_i->scan_rsp);
  if (btm_cb.btm_inq_vars.p_inq_results_cb == nullptr) {
    return;
  }

  (btm_cb.btm_inq_vars.p_inq_results_cb)(&p_i->inq_info.results, nullptr, 0);
}

void btm_api_process_inquiry_result_with_rssi(RawAddress raw_address,
                                              uint8_t page_scan_rep_mode,
                                              DEV_CLASS device_class,
                                              uint16_t clock_offset,
                                              int8_t rssi) {
  tINQ_DB_ENT* p_i = btm_inq_db_find(raw_address);
  if (max_responses_reached()) {
    if (p_i == nullptr || !has_le_device(p_i->inq_info.results.device_type)) {
      return;
    }
  }

  bool update = false;
  if (btm_inq_find_bdaddr(raw_address)) {
    if (btm_cb.btm_inq_vars.inqparms.report_dup && p_i != nullptr &&
        (rssi > p_i->inq_info.results.rssi || p_i->inq_info.results.rssi == 0 ||
         has_classic_device(p_i->inq_info.results.device_type))) {
      update = true;
    }
  }

  bool is_new = true;
  if (p_i == nullptr) {
    p_i = btm_inq_db_new(raw_address);
    CHECK(p_i != nullptr);
  } else if (p_i->inq_count == btm_cb.btm_inq_vars.inq_counter &&
             is_classic_device(p_i->inq_info.results.device_type)) {
    is_new = false;
  }

  p_i->inq_info.results.rssi = rssi;

  if (is_new) {
    p_i->inq_info.results.page_scan_rep_mode = page_scan_rep_mode;
    p_i->inq_info.results.page_scan_per_mode = 0;  // RESERVED
    p_i->inq_info.results.page_scan_mode = 0;      // RESERVED
    p_i->inq_info.results.dev_class[0] = device_class[0];
    p_i->inq_info.results.dev_class[1] = device_class[1];
    p_i->inq_info.results.dev_class[2] = device_class[2];
    p_i->inq_info.results.clock_offset = clock_offset | BTM_CLOCK_OFFSET_VALID;
    p_i->inq_info.results.inq_result_type = BTM_INQ_RESULT_BR;

    p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
    p_i->inq_count = btm_cb.btm_inq_vars.inq_counter;
    p_i->inq_info.appl_knows_rem_name = false;

    if (p_i->inq_count != btm_cb.btm_inq_vars.inq_counter) {
      p_i->inq_info.results.device_type = BT_DEVICE_TYPE_BREDR;
      btm_cb.btm_inq_vars.inq_cmpl_info.num_resp++;
      p_i->scan_rsp = false;
    } else {
      p_i->inq_info.results.device_type |= BT_DEVICE_TYPE_BREDR;
    }
  }

  check_exceeded_responses(p_i->inq_info.results.device_type, p_i->scan_rsp);
  if (btm_cb.btm_inq_vars.p_inq_results_cb == nullptr) {
    return;
  }

  if (is_new || update) {
    (btm_cb.btm_inq_vars.p_inq_results_cb)(&p_i->inq_info.results, nullptr, 0);
  }
}
void btm_api_process_extended_inquiry_result(RawAddress raw_address,
                                             uint8_t page_scan_rep_mode,
                                             DEV_CLASS device_class,
                                             uint16_t clock_offset, int8_t rssi,
                                             const uint8_t* eir_data,
                                             size_t eir_len) {
  tINQ_DB_ENT* p_i = btm_inq_db_find(raw_address);
  if (max_responses_reached()) {
    if (p_i == nullptr || !has_le_device(p_i->inq_info.results.device_type)) {
      return;
    }
  }

  bool update = false;
  if (btm_inq_find_bdaddr(raw_address) && p_i != nullptr) {
    update = true;
  }

  bool is_new = true;
  if (p_i == nullptr) {
    p_i = btm_inq_db_new(raw_address);
  } else if (p_i->inq_count == btm_cb.btm_inq_vars.inq_counter &&
             (p_i->inq_info.results.device_type == BT_DEVICE_TYPE_BREDR)) {
    is_new = false;
  }

  p_i->inq_info.results.rssi = rssi;

  if (is_new) {
    p_i->inq_info.results.page_scan_rep_mode = page_scan_rep_mode;
    p_i->inq_info.results.page_scan_per_mode = 0;  // RESERVED
    p_i->inq_info.results.page_scan_mode = 0;      // RESERVED
    p_i->inq_info.results.dev_class[0] = device_class[0];
    p_i->inq_info.results.dev_class[1] = device_class[1];
    p_i->inq_info.results.dev_class[2] = device_class[2];
    p_i->inq_info.results.clock_offset = clock_offset | BTM_CLOCK_OFFSET_VALID;
    p_i->inq_info.results.inq_result_type = BTM_INQ_RESULT_BR;

    p_i->time_of_resp = bluetooth::common::time_get_os_boottime_ms();
    p_i->inq_count = btm_cb.btm_inq_vars.inq_counter;
    p_i->inq_info.appl_knows_rem_name = false;

    if (p_i->inq_count != btm_cb.btm_inq_vars.inq_counter) {
      p_i->inq_info.results.device_type = BT_DEVICE_TYPE_BREDR;
      btm_cb.btm_inq_vars.inq_cmpl_info.num_resp++;
      p_i->scan_rsp = false;
    } else {
      p_i->inq_info.results.device_type |= BT_DEVICE_TYPE_BREDR;
    }
  }

  check_exceeded_responses(p_i->inq_info.results.device_type, p_i->scan_rsp);
  if (btm_cb.btm_inq_vars.p_inq_results_cb == nullptr) {
    return;
  }

  if (is_new || update) {
    memset(p_i->inq_info.results.eir_uuid, 0,
           BTM_EIR_SERVICE_ARRAY_SIZE * (BTM_EIR_ARRAY_BITS / 8));
    btm_set_eir_uuid(const_cast<uint8_t*>(eir_data), &p_i->inq_info.results);
    uint8_t* p_eir_data = const_cast<uint8_t*>(eir_data);
    (btm_cb.btm_inq_vars.p_inq_results_cb)(&p_i->inq_info.results, p_eir_data,
                                           eir_len);
  }
}

tBTM_STATUS bluetooth::shim::BTM_StartInquiry(tBTM_INQ_PARMS* p_inqparms,
                                              tBTM_INQ_RESULTS_CB* p_results_cb,
                                              tBTM_CMPL_CB* p_cmpl_cb) {
  CHECK(p_inqparms != nullptr);
  CHECK(p_results_cb != nullptr);
  CHECK(p_cmpl_cb != nullptr);

  std::lock_guard<std::mutex> lock(btm_cb_mutex_);

  btm_cb.btm_inq_vars.inq_cmpl_info.num_resp = 0;
  btm_cb.btm_inq_vars.scan_type = INQ_GENERAL;

  Stack::GetInstance()->GetBtm()->StartActiveScanning();
  if (p_inqparms->duration != 0) {
    Stack::GetInstance()->GetBtm()->SetScanningTimer(
        p_inqparms->duration * 1000, common::BindOnce([]() {
          LOG_INFO("%s scanning timeout popped", __func__);
          std::lock_guard<std::mutex> lock(btm_cb_mutex_);
          Stack::GetInstance()->GetBtm()->StopActiveScanning();
        }));
  }

  Stack::GetInstance()->GetBtm()->StartActiveScanning();

  uint8_t classic_mode = p_inqparms->mode & 0x0f;
  if (!Stack::GetInstance()->GetBtm()->SetInquiryFilter(
          classic_mode, p_inqparms->filter_cond_type,
          p_inqparms->filter_cond)) {
    LOG_WARN("%s Unable to set inquiry filter", __func__);
    return BTM_ERR_PROCESSING;
  }

  if (!Stack::GetInstance()->GetBtm()->StartInquiry(
          classic_mode, p_inqparms->duration, p_inqparms->max_resps,
          [](uint16_t status, uint8_t inquiry_mode) {
            LOG_DEBUG("%s Inquiry is complete status:%hd inquiry_mode:%hhd",
                      __func__, status, inquiry_mode);
            btm_cb.btm_inq_vars.inqparms.mode &= ~(inquiry_mode);

            btm_acl_update_busy_level(BTM_BLI_INQ_DONE_EVT);
            if (btm_cb.btm_inq_vars.inq_active) {
              btm_cb.btm_inq_vars.inq_cmpl_info.status = status;
              btm_clear_all_pending_le_entry();
              btm_cb.btm_inq_vars.state = BTM_INQ_INACTIVE_STATE;

              /* Increment so the start of a next inquiry has a new count */
              btm_cb.btm_inq_vars.inq_counter++;

              btm_clr_inq_result_flt();

              if ((status == BTM_SUCCESS) &&
                  controller_get_interface()
                      ->supports_rssi_with_inquiry_results()) {
                btm_sort_inq_result();
              }

              btm_cb.btm_inq_vars.inq_active = BTM_INQUIRY_INACTIVE;
              btm_cb.btm_inq_vars.p_inq_results_cb = nullptr;
              btm_cb.btm_inq_vars.p_inq_cmpl_cb = nullptr;

              if (btm_cb.btm_inq_vars.p_inq_cmpl_cb != nullptr) {
                LOG_DEBUG("%s Sending inquiry completion to upper layer",
                          __func__);
                (btm_cb.btm_inq_vars.p_inq_cmpl_cb)(
                    (tBTM_INQUIRY_CMPL*)&btm_cb.btm_inq_vars.inq_cmpl_info);
                btm_cb.btm_inq_vars.p_inq_cmpl_cb = nullptr;
              }
            }
            if (btm_cb.btm_inq_vars.inqparms.mode == BTM_INQUIRY_NONE &&
                btm_cb.btm_inq_vars.scan_type == INQ_GENERAL) {
              btm_cb.btm_inq_vars.scan_type = INQ_NONE;
            }
          })) {
    LOG_WARN("%s Unable to start inquiry", __func__);
    return BTM_ERR_PROCESSING;
  }

  btm_cb.btm_inq_vars.state = BTM_INQ_ACTIVE_STATE;
  btm_cb.btm_inq_vars.p_inq_cmpl_cb = p_cmpl_cb;
  btm_cb.btm_inq_vars.p_inq_results_cb = p_results_cb;
  btm_cb.btm_inq_vars.inq_active = p_inqparms->mode;

  btm_acl_update_busy_level(BTM_BLI_INQ_EVT);

  return BTM_CMD_STARTED;
}

tBTM_STATUS bluetooth::shim::BTM_SetDiscoverability(uint16_t discoverable_mode,
                                                    uint16_t window,
                                                    uint16_t interval) {
  uint16_t classic_discoverable_mode = discoverable_mode & 0xff;
  uint16_t le_discoverable_mode = discoverable_mode >> 8;

  if (window == 0) window = BTM_DEFAULT_DISC_WINDOW;
  if (interval == 0) interval = BTM_DEFAULT_DISC_INTERVAL;

  switch (le_discoverable_mode) {
    case kDiscoverableModeOff:
      Stack::GetInstance()->GetBtm()->StopAdvertising();
      break;
    case kLimitedDiscoverableMode:
    case kGeneralDiscoverableMode:
      Stack::GetInstance()->GetBtm()->StartAdvertising();
      break;
    default:
      LOG_WARN("%s Unexpected le discoverability mode:%d", __func__,
               le_discoverable_mode);
  }

  switch (classic_discoverable_mode) {
    case kDiscoverableModeOff:
      Stack::GetInstance()->GetBtm()->SetClassicDiscoverabilityOff();
      break;
    case kLimitedDiscoverableMode:
      Stack::GetInstance()->GetBtm()->SetClassicLimitedDiscoverability(
          window, interval);
      break;
    case kGeneralDiscoverableMode:
      Stack::GetInstance()->GetBtm()->SetClassicGeneralDiscoverability(
          window, interval);
      break;
    default:
      LOG_WARN("%s Unexpected classic discoverability mode:%d", __func__,
               classic_discoverable_mode);
  }
  return BTM_SUCCESS;
}

tBTM_STATUS bluetooth::shim::BTM_SetInquiryScanType(uint16_t scan_type) {
  switch (scan_type) {
    case kInterlacedScanType:
      Stack::GetInstance()->GetBtm()->SetInterlacedInquiryScan();
      return BTM_SUCCESS;
      break;
    case kStandardScanType:
      Stack::GetInstance()->GetBtm()->SetStandardInquiryScan();
      return BTM_SUCCESS;
      break;
    default:
      return BTM_ILLEGAL_VALUE;
  }
  return BTM_WRONG_MODE;
}

tBTM_STATUS bluetooth::shim::BTM_BleObserve(bool start, uint8_t duration_sec,
                                            tBTM_INQ_RESULTS_CB* p_results_cb,
                                            tBTM_CMPL_CB* p_cmpl_cb) {
  if (start) {
    CHECK(p_results_cb != nullptr);
    CHECK(p_cmpl_cb != nullptr);

    std::lock_guard<std::mutex> lock(btm_cb_mutex_);

    if (btm_cb.ble_ctr_cb.scan_activity & BTM_LE_OBSERVE_ACTIVE) {
      LOG_WARN("%s Observing already active", __func__);
      return BTM_WRONG_MODE;
    }

    btm_cb.ble_ctr_cb.p_obs_results_cb = p_results_cb;
    btm_cb.ble_ctr_cb.p_obs_cmpl_cb = p_cmpl_cb;
    Stack::GetInstance()->GetBtm()->StartObserving();
    btm_cb.ble_ctr_cb.scan_activity |= BTM_LE_OBSERVE_ACTIVE;

    if (duration_sec != 0) {
      Stack::GetInstance()->GetBtm()->SetObservingTimer(
          duration_sec * 1000, common::BindOnce([]() {
            LOG_DEBUG("%s observing timeout popped", __func__);

            Stack::GetInstance()->GetBtm()->CancelObservingTimer();
            Stack::GetInstance()->GetBtm()->StopObserving();

            std::lock_guard<std::mutex> lock(btm_cb_mutex_);
            btm_cb.ble_ctr_cb.scan_activity &= ~BTM_LE_OBSERVE_ACTIVE;

            if (btm_cb.ble_ctr_cb.p_obs_cmpl_cb) {
              (btm_cb.ble_ctr_cb.p_obs_cmpl_cb)(
                  &btm_cb.btm_inq_vars.inq_cmpl_info);
            }
            btm_cb.ble_ctr_cb.p_obs_results_cb = nullptr;
            btm_cb.ble_ctr_cb.p_obs_cmpl_cb = nullptr;

            btm_cb.btm_inq_vars.inqparms.mode &= ~(BTM_BLE_INQUIRY_MASK);
            btm_cb.btm_inq_vars.scan_type = INQ_NONE;

            btm_acl_update_busy_level(BTM_BLI_INQ_DONE_EVT);

            btm_clear_all_pending_le_entry();
            btm_cb.btm_inq_vars.state = BTM_INQ_INACTIVE_STATE;

            btm_cb.btm_inq_vars.inq_counter++;
            btm_clr_inq_result_flt();
            btm_sort_inq_result();

            btm_cb.btm_inq_vars.inq_active = BTM_INQUIRY_INACTIVE;
            btm_cb.btm_inq_vars.p_inq_results_cb = NULL;
            btm_cb.btm_inq_vars.p_inq_cmpl_cb = NULL;

            if (btm_cb.btm_inq_vars.p_inq_cmpl_cb) {
              (btm_cb.btm_inq_vars.p_inq_cmpl_cb)(
                  (tBTM_INQUIRY_CMPL*)&btm_cb.btm_inq_vars.inq_cmpl_info);
              btm_cb.btm_inq_vars.p_inq_cmpl_cb = nullptr;
            }
          }));
    }
  } else {
    std::lock_guard<std::mutex> lock(btm_cb_mutex_);

    if (!(btm_cb.ble_ctr_cb.scan_activity & BTM_LE_OBSERVE_ACTIVE)) {
      LOG_WARN("%s Observing already inactive", __func__);
    }
    Stack::GetInstance()->GetBtm()->CancelObservingTimer();
    Stack::GetInstance()->GetBtm()->StopObserving();
    btm_cb.ble_ctr_cb.scan_activity &= ~BTM_LE_OBSERVE_ACTIVE;
    Stack::GetInstance()->GetBtm()->StopObserving();
    if (btm_cb.ble_ctr_cb.p_obs_cmpl_cb) {
      (btm_cb.ble_ctr_cb.p_obs_cmpl_cb)(&btm_cb.btm_inq_vars.inq_cmpl_info);
    }
    btm_cb.ble_ctr_cb.p_obs_results_cb = nullptr;
    btm_cb.ble_ctr_cb.p_obs_cmpl_cb = nullptr;
  }
  return BTM_CMD_STARTED;
}

tBTM_STATUS bluetooth::shim::BTM_SetPageScanType(uint16_t scan_type) {
  switch (scan_type) {
    case kInterlacedScanType:
      if (!Stack::GetInstance()->GetBtm()->IsInterlacedScanSupported()) {
        return BTM_MODE_UNSUPPORTED;
      }
      Stack::GetInstance()->GetBtm()->SetInterlacedPageScan();
      return BTM_SUCCESS;
      break;
    case kStandardScanType:
      Stack::GetInstance()->GetBtm()->SetStandardPageScan();
      return BTM_SUCCESS;
      break;
    default:
      return BTM_ILLEGAL_VALUE;
  }
  return BTM_WRONG_MODE;
}

tBTM_STATUS bluetooth::shim::BTM_SetInquiryMode(uint8_t inquiry_mode) {
  switch (inquiry_mode) {
    case kStandardInquiryResult:
      Stack::GetInstance()->GetBtm()->SetStandardInquiryResultMode();
      break;
    case kInquiryResultWithRssi:
      Stack::GetInstance()->GetBtm()->SetInquiryWithRssiResultMode();
      break;
    case kExtendedInquiryResult:
      Stack::GetInstance()->GetBtm()->SetExtendedInquiryResultMode();
      break;
    default:
      return BTM_ILLEGAL_VALUE;
  }
  return BTM_SUCCESS;
}

uint16_t bluetooth::shim::BTM_ReadDiscoverability(uint16_t* p_window,
                                                  uint16_t* p_interval) {
  DiscoverabilityState state =
      Stack::GetInstance()->GetBtm()->GetClassicDiscoverabilityState();

  if (p_interval) *p_interval = state.interval;
  if (p_window) *p_window = state.window;

  return state.mode;
}

tBTM_STATUS bluetooth::shim::BTM_CancelPeriodicInquiry(void) {
  Stack::GetInstance()->GetBtm()->CancelPeriodicInquiry();
  return BTM_SUCCESS;
}

tBTM_STATUS bluetooth::shim::BTM_SetConnectability(uint16_t page_mode,
                                                   uint16_t window,
                                                   uint16_t interval) {
  uint16_t classic_connectible_mode = page_mode & 0xff;
  uint16_t le_connectible_mode = page_mode >> 8;

  if (!window) window = BTM_DEFAULT_CONN_WINDOW;
  if (!interval) interval = BTM_DEFAULT_CONN_INTERVAL;

  switch (le_connectible_mode) {
    case kConnectibleModeOff:
      Stack::GetInstance()->GetBtm()->StopConnectability();
      break;
    case kConnectibleModeOn:
      Stack::GetInstance()->GetBtm()->StartConnectability();
      break;
    default:
      return BTM_ILLEGAL_VALUE;
      break;
  }

  switch (classic_connectible_mode) {
    case kConnectibleModeOff:
      Stack::GetInstance()->GetBtm()->SetClassicConnectibleOff();
      break;
    case kConnectibleModeOn:
      Stack::GetInstance()->GetBtm()->SetClassicConnectibleOn();
      break;
    default:
      return BTM_ILLEGAL_VALUE;
      break;
  }
  return BTM_SUCCESS;
}

uint16_t bluetooth::shim::BTM_ReadConnectability(uint16_t* p_window,
                                                 uint16_t* p_interval) {
  ConnectabilityState state =
      Stack::GetInstance()->GetBtm()->GetClassicConnectabilityState();

  if (p_window) *p_window = state.window;
  if (p_interval) *p_interval = state.interval;

  return state.mode;
}

uint16_t bluetooth::shim::BTM_IsInquiryActive(void) {
  if (Stack::GetInstance()->GetBtm()->IsLimitedInquiryActive()) {
    return BTM_LIMITED_INQUIRY_ACTIVE;
  } else if (Stack::GetInstance()->GetBtm()->IsGeneralInquiryActive()) {
    return BTM_GENERAL_INQUIRY_ACTIVE;
  } else if (Stack::GetInstance()->GetBtm()->IsGeneralPeriodicInquiryActive() ||
             Stack::GetInstance()->GetBtm()->IsLimitedPeriodicInquiryActive()) {
    return BTM_PERIODIC_INQUIRY_ACTIVE;
  }
  return BTM_INQUIRY_INACTIVE;
}

tBTM_STATUS bluetooth::shim::BTM_CancelInquiry(void) {
  LOG_DEBUG("%s Cancel inquiry", __func__);
  Stack::GetInstance()->GetBtm()->CancelInquiry();

  btm_cb.btm_inq_vars.state = BTM_INQ_INACTIVE_STATE;
  btm_clr_inq_result_flt();

  Stack::GetInstance()->GetBtm()->CancelScanningTimer();
  Stack::GetInstance()->GetBtm()->StopActiveScanning();

  btm_cb.ble_ctr_cb.scan_activity &= ~BTM_BLE_INQUIRY_MASK;

  btm_cb.btm_inq_vars.inqparms.mode &=
      ~(btm_cb.btm_inq_vars.inqparms.mode & BTM_BLE_INQUIRY_MASK);

  btm_acl_update_busy_level(BTM_BLI_INQ_DONE_EVT);
  /* Ignore any stray or late complete messages if the inquiry is not active */
  if (btm_cb.btm_inq_vars.inq_active) {
    btm_cb.btm_inq_vars.inq_cmpl_info.status = BTM_SUCCESS;
    btm_clear_all_pending_le_entry();

    if (controller_get_interface()->supports_rssi_with_inquiry_results()) {
      btm_sort_inq_result();
    }

    btm_cb.btm_inq_vars.inq_active = BTM_INQUIRY_INACTIVE;
    btm_cb.btm_inq_vars.p_inq_results_cb = nullptr;
    btm_cb.btm_inq_vars.p_inq_cmpl_cb = nullptr;
    btm_cb.btm_inq_vars.inq_counter++;

    if (btm_cb.btm_inq_vars.p_inq_cmpl_cb != nullptr) {
      LOG_DEBUG("%s Sending cancel inquiry completion to upper layer",
                __func__);
      (btm_cb.btm_inq_vars.p_inq_cmpl_cb)(
          (tBTM_INQUIRY_CMPL*)&btm_cb.btm_inq_vars.inq_cmpl_info);
      btm_cb.btm_inq_vars.p_inq_cmpl_cb = nullptr;
    }
  }
  if (btm_cb.btm_inq_vars.inqparms.mode == BTM_INQUIRY_NONE &&
      btm_cb.btm_inq_vars.scan_type == INQ_GENERAL) {
    btm_cb.btm_inq_vars.scan_type = INQ_NONE;
  }
  return BTM_SUCCESS;
}

tBTM_STATUS bluetooth::shim::BTM_ReadRemoteDeviceName(
    const RawAddress& raw_address, tBTM_CMPL_CB* callback,
    tBT_TRANSPORT transport) {
  CHECK(callback != nullptr);
  tBTM_STATUS status = BTM_NO_RESOURCES;

  switch (transport) {
    case BT_TRANSPORT_LE:
      status = Stack::GetInstance()->GetBtm()->ReadLeRemoteDeviceName(
          raw_address, callback);
      break;
    case BT_TRANSPORT_BR_EDR:
      status = Stack::GetInstance()->GetBtm()->ReadClassicRemoteDeviceName(
          raw_address, callback);
      break;
    default:
      LOG_WARN("%s Unspecified transport:%d", __func__, transport);
      break;
  }
  return status;
}

tBTM_STATUS bluetooth::shim::BTM_CancelRemoteDeviceName(void) {
  return Stack::GetInstance()->GetBtm()->CancelAllReadRemoteDeviceName();
}

tBTM_INQ_INFO* bluetooth::shim::BTM_InqDbRead(const RawAddress& p_bda) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return nullptr;
}

tBTM_INQ_INFO* bluetooth::shim::BTM_InqDbFirst(void) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return nullptr;
}

tBTM_INQ_INFO* bluetooth::shim::BTM_InqDbNext(tBTM_INQ_INFO* p_cur) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_cur != nullptr);
  return nullptr;
}

tBTM_STATUS bluetooth::shim::BTM_ClearInqDb(const RawAddress* p_bda) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  if (p_bda == nullptr) {
    // clear all entries
  } else {
    // clear specific entry
  }
  return BTM_NO_RESOURCES;
}

tBTM_STATUS bluetooth::shim::BTM_WriteEIR(BT_HDR* p_buff) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_buff != nullptr);
  return BTM_NO_RESOURCES;
}

bool bluetooth::shim::BTM_HasEirService(const uint32_t* p_eir_uuid,
                                        uint16_t uuid16) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_eir_uuid != nullptr);
  return false;
}

tBTM_EIR_SEARCH_RESULT bluetooth::shim::BTM_HasInquiryEirService(
    tBTM_INQ_RESULTS* p_results, uint16_t uuid16) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_results != nullptr);
  return BTM_EIR_UNKNOWN;
}

void bluetooth::shim::BTM_AddEirService(uint32_t* p_eir_uuid, uint16_t uuid16) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_eir_uuid != nullptr);
}

void bluetooth::shim::BTM_RemoveEirService(uint32_t* p_eir_uuid,
                                           uint16_t uuid16) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_eir_uuid != nullptr);
}

uint8_t bluetooth::shim::BTM_GetEirSupportedServices(uint32_t* p_eir_uuid,
                                                     uint8_t** p,
                                                     uint8_t max_num_uuid16,
                                                     uint8_t* p_num_uuid16) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_eir_uuid != nullptr);
  CHECK(p != nullptr);
  CHECK(*p != nullptr);
  CHECK(p_num_uuid16 != nullptr);
  return BTM_NO_RESOURCES;
}

uint8_t bluetooth::shim::BTM_GetEirUuidList(uint8_t* p_eir, size_t eir_len,
                                            uint8_t uuid_size,
                                            uint8_t* p_num_uuid,
                                            uint8_t* p_uuid_list,
                                            uint8_t max_num_uuid) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_eir != nullptr);
  CHECK(p_num_uuid != nullptr);
  CHECK(p_uuid_list != nullptr);
  return 0;
}

bool bluetooth::shim::BTM_SecAddBleDevice(const RawAddress& bd_addr,
                                          BD_NAME bd_name,
                                          tBT_DEVICE_TYPE dev_type,
                                          tBLE_ADDR_TYPE addr_type) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return false;
}

bool bluetooth::shim::BTM_SecAddBleKey(const RawAddress& bd_addr,
                                       tBTM_LE_KEY_VALUE* p_le_key,
                                       tBTM_LE_KEY_TYPE key_type) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_le_key != nullptr);
  return false;
}

void bluetooth::shim::BTM_BleLoadLocalKeys(uint8_t key_type,
                                           tBTM_BLE_LOCAL_KEYS* p_key) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_key != nullptr);
}

static Octet16 bogus_root;

/** Returns local device encryption root (ER) */
const Octet16& bluetooth::shim::BTM_GetDeviceEncRoot() {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return bogus_root;
}

/** Returns local device identity root (IR). */
const Octet16& bluetooth::shim::BTM_GetDeviceIDRoot() {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return bogus_root;
}

/** Return local device DHK. */
const Octet16& bluetooth::shim::BTM_GetDeviceDHK() {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return bogus_root;
}

void bluetooth::shim::BTM_ReadConnectionAddr(const RawAddress& remote_bda,
                                             RawAddress& local_conn_addr,
                                             tBLE_ADDR_TYPE* p_addr_type) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_addr_type != nullptr);
}

bool bluetooth::shim::BTM_ReadRemoteConnectionAddr(
    const RawAddress& pseudo_addr, RawAddress& conn_addr,
    tBLE_ADDR_TYPE* p_addr_type) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_addr_type != nullptr);
  return false;
}

void bluetooth::shim::BTM_SecurityGrant(const RawAddress& bd_addr,
                                        uint8_t res) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_BleOobDataReply(const RawAddress& bd_addr,
                                          uint8_t res, uint8_t len,
                                          uint8_t* p_data) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_data != nullptr);
}

void bluetooth::shim::BTM_BleSecureConnectionOobDataReply(
    const RawAddress& bd_addr, uint8_t* p_c, uint8_t* p_r) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_c != nullptr);
  CHECK(p_r != nullptr);
}

void bluetooth::shim::BTM_BleSetConnScanParams(uint32_t scan_interval,
                                               uint32_t scan_window) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_BleSetPrefConnParams(const RawAddress& bd_addr,
                                               uint16_t min_conn_int,
                                               uint16_t max_conn_int,
                                               uint16_t slave_latency,
                                               uint16_t supervision_tout) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_ReadDevInfo(const RawAddress& remote_bda,
                                      tBT_DEVICE_TYPE* p_dev_type,
                                      tBLE_ADDR_TYPE* p_addr_type) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_dev_type != nullptr);
  CHECK(p_addr_type != nullptr);
}

bool bluetooth::shim::BTM_ReadConnectedTransportAddress(
    RawAddress* remote_bda, tBT_TRANSPORT transport) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(remote_bda != nullptr);
  return false;
}

void bluetooth::shim::BTM_BleReceiverTest(uint8_t rx_freq,
                                          tBTM_CMPL_CB* p_cmd_cmpl_cback) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_cmd_cmpl_cback != nullptr);
}

void bluetooth::shim::BTM_BleTransmitterTest(uint8_t tx_freq,
                                             uint8_t test_data_len,
                                             uint8_t packet_payload,
                                             tBTM_CMPL_CB* p_cmd_cmpl_cback) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_cmd_cmpl_cback != nullptr);
}

void bluetooth::shim::BTM_BleTestEnd(tBTM_CMPL_CB* p_cmd_cmpl_cback) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_cmd_cmpl_cback != nullptr);
}

bool bluetooth::shim::BTM_UseLeLink(const RawAddress& raw_address) {
  return Stack::GetInstance()->GetBtm()->UseLeLink(raw_address);
}

tBTM_STATUS bluetooth::shim::BTM_SetBleDataLength(const RawAddress& bd_addr,
                                                  uint16_t tx_pdu_length) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return BTM_NO_RESOURCES;
}

void bluetooth::shim::BTM_BleReadPhy(
    const RawAddress& bd_addr,
    base::Callback<void(uint8_t tx_phy, uint8_t rx_phy, uint8_t status)> cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_BleSetPhy(const RawAddress& bd_addr, uint8_t tx_phys,
                                    uint8_t rx_phys, uint16_t phy_options) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

bool bluetooth::shim::BTM_BleDataSignature(const RawAddress& bd_addr,
                                           uint8_t* p_text, uint16_t len,
                                           BLE_SIGNATURE signature) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_text != nullptr);
  return false;
}

bool bluetooth::shim::BTM_BleVerifySignature(const RawAddress& bd_addr,
                                             uint8_t* p_orig, uint16_t len,
                                             uint32_t counter,
                                             uint8_t* p_comp) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_orig != nullptr);
  CHECK(p_comp != nullptr);
  return false;
}

bool bluetooth::shim::BTM_GetLeSecurityState(const RawAddress& bd_addr,
                                             uint8_t* p_le_dev_sec_flags,
                                             uint8_t* p_le_key_size) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  CHECK(p_le_dev_sec_flags != nullptr);
  CHECK(p_le_key_size != nullptr);
  return false;
}

bool bluetooth::shim::BTM_BleSecurityProcedureIsRunning(
    const RawAddress& bd_addr) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return false;
}

uint8_t bluetooth::shim::BTM_BleGetSupportedKeySize(const RawAddress& bd_addr) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
  return 0;
}

/**
 * This function update(add,delete or clear) the adv local name filtering
 * condition.
 */
void bluetooth::shim::BTM_LE_PF_local_name(tBTM_BLE_SCAN_COND_OP action,
                                           tBTM_BLE_PF_FILT_INDEX filt_index,
                                           std::vector<uint8_t> name,
                                           tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_srvc_data(tBTM_BLE_SCAN_COND_OP action,
                                          tBTM_BLE_PF_FILT_INDEX filt_index) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_manu_data(
    tBTM_BLE_SCAN_COND_OP action, tBTM_BLE_PF_FILT_INDEX filt_index,
    uint16_t company_id, uint16_t company_id_mask, std::vector<uint8_t> data,
    std::vector<uint8_t> data_mask, tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_srvc_data_pattern(
    tBTM_BLE_SCAN_COND_OP action, tBTM_BLE_PF_FILT_INDEX filt_index,
    std::vector<uint8_t> data, std::vector<uint8_t> data_mask,
    tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_addr_filter(tBTM_BLE_SCAN_COND_OP action,
                                            tBTM_BLE_PF_FILT_INDEX filt_index,
                                            tBLE_BD_ADDR addr,
                                            tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_uuid_filter(tBTM_BLE_SCAN_COND_OP action,
                                            tBTM_BLE_PF_FILT_INDEX filt_index,
                                            tBTM_BLE_PF_COND_TYPE filter_type,
                                            const bluetooth::Uuid& uuid,
                                            tBTM_BLE_PF_LOGIC_TYPE cond_logic,
                                            const bluetooth::Uuid& uuid_mask,
                                            tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_set(tBTM_BLE_PF_FILT_INDEX filt_index,
                                    std::vector<ApcfCommand> commands,
                                    tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_LE_PF_clear(tBTM_BLE_PF_FILT_INDEX filt_index,
                                      tBTM_BLE_PF_CFG_CBACK cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_BleAdvFilterParamSetup(
    int action, tBTM_BLE_PF_FILT_INDEX filt_index,
    std::unique_ptr<btgatt_filt_param_setup_t> p_filt_params,
    tBTM_BLE_PF_PARAM_CB cb) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_BleUpdateAdvFilterPolicy(tBTM_BLE_AFP adv_policy) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::BTM_BleEnableDisableFilterFeature(
    uint8_t enable, tBTM_BLE_PF_STATUS_CBACK p_stat_cback) {
  LOG_INFO("UNIMPLEMENTED %s", __func__);
}

uint8_t bluetooth::shim::BTM_BleMaxMultiAdvInstanceCount() {
  return Stack::GetInstance()->GetBtm()->GetNumberOfAdvertisingInstances();
}

bool bluetooth::shim::BTM_BleLocalPrivacyEnabled(void) {
  return controller_get_interface()->supports_ble_privacy();
}

tBTM_STATUS bluetooth::shim::BTM_SecBond(const RawAddress& bd_addr,
                                         tBLE_ADDR_TYPE addr_type,
                                         tBT_TRANSPORT transport,
                                         int device_type) {
  return Stack::GetInstance()->GetBtm()->CreateBond(bd_addr, addr_type,
                                                    transport, device_type);
}

bool bluetooth::shim::BTM_SecRegister(const tBTM_APPL_INFO* p_cb_info) {
  CHECK(p_cb_info != nullptr);
  LOG_DEBUG("%s Registering security application", __func__);

  if (p_cb_info->p_authorize_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s authorize_callback", __func__);
  }

  if (p_cb_info->p_pin_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s pin_callback", __func__);
  }

  if (p_cb_info->p_link_key_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s link_key_callback", __func__);
  }

  if (p_cb_info->p_auth_complete_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s auth_complete_callback", __func__);
  }

  if (p_cb_info->p_bond_cancel_cmpl_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s bond_cancel_complete_callback", __func__);
  }

  if (p_cb_info->p_le_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s le_callback", __func__);
  }

  if (p_cb_info->p_le_key_callback == nullptr) {
    LOG_INFO("UNIMPLEMENTED %s le_key_callback", __func__);
  }

  return true;
}

tBTM_STATUS bluetooth::shim::BTM_SecBondCancel(const RawAddress& bd_addr) {
  if (Stack::GetInstance()->GetBtm()->CancelBond(bd_addr)) {
    return BTM_SUCCESS;
  } else {
    return BTM_UNKNOWN_ADDR;
  }
}

bool bluetooth::shim::BTM_SecDeleteDevice(const RawAddress& bd_addr) {
  return Stack::GetInstance()->GetBtm()->RemoveBond(bd_addr);
}

uint16_t bluetooth::shim::BTM_GetHCIConnHandle(const RawAddress& remote_bda,
                                               tBT_TRANSPORT transport) {
  return Stack::GetInstance()->GetBtm()->GetAclHandle(remote_bda, transport);
}

static void remote_name_request_complete_noop(void* p_name){
    // Should notify BTM_Sec, but we should use GD SMP.
};

void bluetooth::shim::SendRemoteNameRequest(const RawAddress& raw_address) {
  Stack::GetInstance()->GetBtm()->ReadClassicRemoteDeviceName(
      raw_address, remote_name_request_complete_noop);
}

tBTM_STATUS bluetooth::shim::btm_sec_mx_access_request(
    const RawAddress& bd_addr, uint16_t psm, bool is_originator,
    uint32_t mx_proto_id, uint32_t mx_chan_id, tBTM_SEC_CBACK* p_callback,
    void* p_ref_data) {
  // Security has already been fulfilled by the l2cap connection, so reply back
  // that everything is totally fine and legit and definitely not two kids in a
  // trenchcoat

  if (p_callback) {
    (*p_callback)(&bd_addr, false, p_ref_data, BTM_SUCCESS);
  }
  return BTM_SUCCESS;
}

tBTM_STATUS bluetooth::shim::BTM_SetEncryption(const RawAddress& bd_addr,
                                               tBT_TRANSPORT transport,
                                               tBTM_SEC_CBACK* p_callback,
                                               void* p_ref_data,
                                               tBTM_BLE_SEC_ACT sec_act) {
  // When we just bond a device, encryption is already done
  (*p_callback)(&bd_addr, transport, p_ref_data, BTM_SUCCESS);

  // TODO(hsz): Re-encrypt the link after first bonded

  return BTM_SUCCESS;
}