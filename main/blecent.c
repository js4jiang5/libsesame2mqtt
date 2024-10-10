#include "blecent.h"
#include "candy.h"
#include "esp_central.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "mqtt_section.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "ssm_cmd.h"
static const char * TAG = "blecent.c";

static const ble_uuid_t * ssm_svc_uuid = BLE_UUID16_DECLARE(0xFD81); // https://github.com/CANDY-HOUSE/Sesame_BluetoothAPI_document/blob/master/SesameOS3/1_advertising.md
static const ble_uuid_t * ssm_chr_uuid = BLE_UUID128_DECLARE(0x3e, 0x99, 0x76, 0xc6, 0xb4, 0xdb, 0xd3, 0xb6, 0x56, 0x98, 0xae, 0xa5, 0x02, 0x00, 0x86, 0x16);
static const ble_uuid_t * ssm_ntf_uuid = BLE_UUID128_DECLARE(0x3e, 0x99, 0x76, 0xc6, 0xb4, 0xdb, 0xd3, 0xb6, 0x56, 0x98, 0xae, 0xa5, 0x03, 0x00, 0x86, 0x16);

static int ble_gap_connect_event(struct ble_gap_event * event, void * arg);
// static int ble_gap_connect_event_tch(struct ble_gap_event * event, void * arg);

static int ssm_enable_notify(uint16_t conn_handle) {
	const struct peer_dsc * dsc;
	const struct peer * peer = peer_find(conn_handle);
	dsc = peer_dsc_find_uuid(peer, ssm_svc_uuid, ssm_ntf_uuid, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
	if (dsc == NULL) {
		ESP_LOGE(TAG, "Error: Peer lacks a CCCD for the Unread Alert Status characteristic\n");
		goto err;
	}
	uint8_t value[2] = { 0x01, 0x00 };
	int rc = ble_gattc_write_flat(conn_handle, dsc->dsc.handle, value, sizeof(value), NULL, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "Error: Failed to subscribe to characteristic; rc=%d\n", rc);
		goto err;
	}
	ESP_LOGW(TAG, "Enable notify success!!");
	return ESP_OK;
err:
	return ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM); /* Terminate the connection. */
}

static void service_disc_complete(const struct peer * peer, int status, void * arg) {
	// sesame * ssm = (sesame *) arg;
	if (status != 0) {
		ESP_LOGE(TAG, "Error: Service discovery failed; status=%d conn_handle=%d\n", status, peer->conn_handle);
		ble_gap_terminate(peer->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
		return;
	}
	ESP_LOGI(TAG, "Service discovery complete conn_handle=%d\n", peer->conn_handle);
	ssm_enable_notify(peer->conn_handle);
}

static int ble_gap_event_connect_handle(struct ble_gap_event * event, sesame * ssm) {
	if (event->connect.status != 0) {
		ESP_LOGE(TAG, "Error: Connection failed; status=%d\n", event->connect.status);
		ble_hs_cfg.sync_cb(); // resume BLE scan
		return ESP_FAIL;
	}
	static struct ble_gap_conn_desc desc;
	int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
	assert(rc == 0);
	print_conn_desc(&desc);
	rc = peer_add(event->connect.conn_handle);
	if (rc != 0) {
		ESP_LOGE(TAG, "Failed to add peer for %s with conn_id = %d; rc=%d\n", SSM_PRODUCT_TYPE_STR(ssm->product_type), event->connect.conn_handle, rc);
		ble_hs_cfg.sync_cb(); // resume BLE scan
		return ESP_FAIL;
	}
	ssm->device_status = SSM_CONNECTED;		   // set the device status
	ssm->conn_id = event->connect.conn_handle; // save the connection handle
	ESP_LOGW(TAG, "Connect %s success handle=%d", SSM_PRODUCT_TYPE_STR(ssm->product_type), ssm->conn_id);
	rc = peer_disc_all(event->connect.conn_handle, service_disc_complete, ssm);
	if (rc != 0) {
		ESP_LOGE(TAG, "Failed to discover services; rc=%d\n", rc);
		return ESP_FAIL;
	}
	return ESP_OK;
}

static void reconnect_ssm(sesame * ssm) {
	ble_addr_t addr;
	addr.type = BLE_ADDR_RANDOM;
	memcpy(addr.val, ssm->addr, 6);
	int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, NULL, ble_gap_connect_event, ssm);
	if (rc != 0) {
		ESP_LOGE(TAG, "Error: Failed to connect to device; rc=%d\n", rc);
		if ((ssm->product_type == SESAME_5 || ssm->product_type == SESAME_5_PRO) && ssm->mqtt_discovery_done) { // disconnect after MQTT discovery is done
			esp_restart(); // 20241010
		}
		return;
	}
}

void disconnect(sesame * ssm) {
	if (ssm->device_status > SSM_DISCONNECTED) { // disconnect if is connected
		ssm->disconnect_forever = 1;
		ble_gap_terminate(ssm->conn_id, BLE_ERR_REM_USER_CONN_TERM); /* Terminate the connection. */
		//vTaskDelay(600 / portTICK_PERIOD_MS);
	}
}

void reconnect(sesame * ssm) {
	if (ssm->device_status <= SSM_DISCONNECTED) { // reconnect if is disconnected
		ble_gap_disc_cancel(); // stop BLE scan, added on 2024.06.15 by JS
		reconnect_ssm(ssm);
	} else { // disconnect to trigger reconnect automatically
		ble_gap_terminate(ssm->conn_id, BLE_ERR_REM_USER_CONN_TERM); /* Terminate the connection. */
		//vTaskDelay(600 / portTICK_PERIOD_MS);
	}
}

static int ble_gap_connect_event(struct ble_gap_event * event, void * arg) {
	ESP_LOGI(TAG, "[ble_gap_connect_event: %d]", event->type);
	sesame * ssm = (sesame *) arg;
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		return ble_gap_event_connect_handle(event, ssm);

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGW(TAG, "%s disconnect; reason=%d ", SSM_PRODUCT_TYPE_STR(ssm->product_type), event->disconnect.reason);
		ssm->device_status = SSM_DISCONNECTED;
		ssm->conn_id = 0xFF;
		print_conn_desc(&event->disconnect.conn);
		peer_delete(event->disconnect.conn.conn_handle);		
		if (ssm->disconnect_forever) {
			ssm->disconnect_forever = 0;
			return ESP_OK;
		}
		if (event->disconnect.reason == 531) { // Sesame teminate the connection. Should be caused by device reset
			esp_restart();
		} else if (event->disconnect.reason == 12) { // Sesame disconnect due to BLE_HS_ECONTROLLER
			ESP_LOGE(TAG, "restart ESP, reason = 12");
			esp_restart();
		}

		ble_gap_disc_cancel(); // stop BLE scan, added on 2024.04.17 by JS
		vTaskDelay(600 / portTICK_PERIOD_MS);
		reconnect_ssm(ssm);
		return ESP_OK;

	case BLE_GAP_EVENT_CONN_UPDATE_REQ:
		ESP_LOGI(TAG, "connection update request event; conn_handle=%d itvl_min=%d itvl_max=%d latency=%d supervision_timoeut=%d min_ce_len=%d max_ce_len=%d\n", event->conn_update_req.conn_handle, event->conn_update_req.peer_params->itvl_min,
				 event->conn_update_req.peer_params->itvl_max, event->conn_update_req.peer_params->latency, event->conn_update_req.peer_params->supervision_timeout, event->conn_update_req.peer_params->min_ce_len,
				 event->conn_update_req.peer_params->max_ce_len);
		*event->conn_update_req.self_params = *event->conn_update_req.peer_params;
		return ESP_OK;

	case BLE_GAP_EVENT_CONN_UPDATE:
		ESP_LOGI(TAG, "connect update success");
		return ESP_OK;

	case BLE_GAP_EVENT_NOTIFY_RX:
		ssm_ble_receiver(ssm, event->notify_rx.om->om_data, event->notify_rx.om->om_len);
		if (ssm->update_status) {
			sesame_update();
			mqtt_discovery();
			mqtt_subscribe();
		}
		ble_hs_cfg.sync_cb(); // resume BLE scan
		return ESP_OK;

	case BLE_GAP_EVENT_DISC_COMPLETE:
		ESP_LOGW(TAG, "BLE_GAP_EVENT_DISC_COMPLETE");
		return ESP_OK;

	default:
		return ESP_OK;
	}
}

static void ssm_scan_connect(const struct ble_hs_adv_fields * fields, void * disc) {
	ble_addr_t * addr = &((struct ble_gap_disc_desc *) disc)->addr;
	int8_t rssi = ((struct ble_gap_disc_desc *) disc)->rssi;
	struct ssm_env_tag * p_tag = NULL;

	if (timer_1min()) { // 1 min timeout, check RSSI
		char topic[80] = "";
		char payload[8] = "";
		for (int i_ssm = 0; i_ssm < cnt_ssms; i_ssm++) {
			sesame *ssm = &(p_ssms_env + i_ssm)->ssm;
			memset(topic, 0, sizeof(topic));
			sprintf(topic, "homeassistant/%s/state/rssi", ssm->topic); 
			if (ssm->is_alive) { // MQTT publish RSSI if value is changed
				if (ssm->rssi_changed) { 
					memset(payload, 0, sizeof(payload));
					sprintf(payload, "%d", ssm->rssi);
					int msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1); // QOS 2, retain 0
					ESP_LOGI(TAG, "sent mqtt rssi for %s, msg_id=%d", ssm->topic, msg_id);
					wait_published(msg_id);
				}
			} else {
				if (ssm->rssi != -128) { // MQTT publish RSSI not available if never published
					ssm->rssi = -128;
					int msg_id = esp_mqtt_client_publish(client_ssm, topic, "None", 0, 2, 1); // QOS 2, retain 0
					ESP_LOGI(TAG, "sent mqtt rssi not available for %s, msg_id=%d", ssm->topic, msg_id);
					wait_published(msg_id);
				}
			}
			ssm->is_alive = 0; // reset
			ssm->rssi_changed = 0;
		}
	}

	if (fields->mfg_data_len >= 5 && fields->mfg_data[0] == 0x5A && fields->mfg_data[1] == 0x05) { // is SSM
		for (int n = 0; n < cnt_ssms; n++) {
			sesame *ssm = &(p_ssms_env + n)->ssm;													   // skip if the device was discovered already
			if (memcmp(ssm->addr, addr->val, sizeof(uint8_t) * 6) == 0) {
				if ((ssm->product_type == SESAME_5 || ssm->product_type == SESAME_5_PRO) && ssm->device_status < SSM_LOGGIN) { // if Sesame 5 or Sesame 5 PRO is logout unexpectedly, restart ESP32
					esp_restart(); // 20241009
				}
				if (++ssm->cnt_discovery > 128) { // accumulate the number of times this device has been discovered
					ssm->cnt_discovery = 128;	   // avoid saturation and wrap around
				}
				if (!ssm->rssi_changed && rssi != ssm->rssi) {
					ssm->rssi_changed = 1;
				}
				ssm->rssi = rssi;
				ssm->is_alive = 1;
				return;
			}
		}
		if (((struct ble_gap_disc_desc *) disc)->rssi < -95) { // RSSI threshold
			return;
		}
		p_tag = p_ssms_env + cnt_ssms;
		p_tag->ssm.rssi = rssi;
		memcpy(p_tag->ssm.addr, addr->val, 6);
		if (fields->mfg_data[2] == 5) { // Sesame Lock
			p_tag->ssm.product_type = SESAME_5;
		} else if (fields->mfg_data[2] == 6) { // Sesame Bike 2
			p_tag->ssm.product_type = SESAME_BIKE_2;
		} else if (fields->mfg_data[2] == 7) { // Sesame 5 PRO
			p_tag->ssm.product_type = SESAME_5_PRO;
		} else if (fields->mfg_data[2] == 9) { // Sesame Touch PRO
			p_tag->ssm.product_type = SESAME_TOUCH_PRO;
		} else if (fields->mfg_data[2] == 10) { // Sesame Touch
			p_tag->ssm.product_type = SESAME_TOUCH;
		} else { // Not supported
			return;
		}

		if (fields->mfg_data[4] == 0x00) { // unregistered SSM
			ESP_LOGW(TAG, "find unregistered %s", SSM_PRODUCT_TYPE_STR(p_tag->ssm.product_type));
			if (p_tag->ssm.device_status == SSM_NOUSE) {
				p_tag->ssm.device_status = SSM_DISCONNECTED;
				p_tag->ssm.conn_id = 0xFF;
			}
			memcpy(p_tag->ssm.device_uuid, &fields->mfg_data[5], 16); // save device UUID
		} else {													  // registered SSM
			ESP_LOGW(TAG, "find registered %s", SSM_PRODUCT_TYPE_STR(p_tag->ssm.product_type));
			if (ssm_read_nvs(&p_tag->ssm) == 0) { // NVS read fail
				return;
			}
		}
		ble_gap_disc_cancel(); // stop scan
		ESP_LOGW(TAG, "Connect %s addr=%s addrType=%d", SSM_PRODUCT_TYPE_STR(p_tag->ssm.product_type), addr_str(addr->val), addr->type);
		int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, 30000, NULL, ble_gap_connect_event, &p_tag->ssm);
		if (rc != 0) {
			ESP_LOGE(TAG, "Error: Failed to connect to device; rc=%d\n", rc);
			ble_hs_cfg.sync_cb(); // resume BLE scan
			return;
		}
	} else {
		return; // not SSM
	}
}

static int ble_gap_disc_event(struct ble_gap_event * event, void * arg) {
	// ESP_LOG_BUFFER_HEX_LEVEL("[find_device_mac]", event->disc.addr.val, 6, ESP_LOG_WARN);
	struct ble_hs_adv_fields fields;
	int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
	if (rc != 0) {
		return ESP_FAIL;
	}
	ssm_scan_connect(&fields, &event->disc);
	return ESP_OK;
}

static void blecent_scan(void) {
	if (ble_gap_disc_active()) { // blecent scan is ongoing
		return;
	}
	ESP_LOGI(TAG, "[blecent_scan][START]");
	struct ble_gap_disc_params disc_params;
	disc_params.filter_duplicates = 0;
	disc_params.passive = 1;
	disc_params.itvl = 0;
	disc_params.window = 0;
	disc_params.filter_policy = 0;
	disc_params.limited = 0;

	int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_disc_event, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "Error initiating GAP discovery procedure; rc=0x%x\n", rc);
	}
}

static void blecent_host_task(void * param) {
	ESP_LOGI(TAG, "BLE Host Task Started");
	nimble_port_run();
	nimble_port_freertos_deinit();
}

void esp_ble_gatt_write(sesame * ssm, uint8_t * value, uint16_t length) {
	const struct peer * peer = peer_find(ssm->conn_id);
	const struct peer_chr * chr = peer_chr_find_uuid(peer, ssm_svc_uuid, ssm_chr_uuid);
	if (chr == NULL) {
		ESP_LOGE(TAG, "Error: Peer doesn't have the subscribable characteristic\n");
		return;
	}
	int rc = ble_gattc_write_flat(ssm->conn_id, chr->chr.val_handle, value, length, NULL, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "Error: Failed to write to the subscribable characteristic; rc=%d\n", rc);
	}
}

void esp_ble_init(void) {
	esp_err_t ret = nimble_port_init();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to init nimble %d ", ret);
		esp_mqtt_client_publish(client_ssm, "12345", "Failed to init nimble", 0, 2, 0); // QOS 2, retain 1
		return;
	}
	ble_hs_cfg.sync_cb = blecent_scan;
	int rc = peer_init(SSM_MAX_NUM, 64, 64, 64);
	assert(rc == 0);
	nimble_port_freertos_init(blecent_host_task);
	ESP_LOGI(TAG, "[esp_ble_init][SUCCESS]");
}

void sesame_update(void) {
	if (cnt_ssms == 2 && cnt_unregistered_ssms > 0) { // automaticly add sesame for touch if there are only 2 sesame devices and at least one of them is newly registered
		sesame *ssm1 = &p_ssms_env->ssm, *ssm2 = &(p_ssms_env + 1)->ssm;
		sesame *tch = NULL, *ssm = NULL;
		cnt_unregistered_ssms = 0;
		if ((ssm1->product_type == SESAME_5 || ssm1->product_type == SESAME_5_PRO) && (ssm2->product_type == SESAME_TOUCH || ssm2->product_type == SESAME_TOUCH_PRO)) {
			tch = ssm2; ssm = ssm1;
		} else if ((ssm2->product_type == SESAME_5 || ssm2->product_type == SESAME_5_PRO) && (ssm1->product_type == SESAME_TOUCH || ssm1->product_type == SESAME_TOUCH_PRO)) {
			tch = ssm1; ssm = ssm2;
			wake_up(tch);
		}
		if (tch != NULL && ssm != NULL) {
			tch_add_sesame(tch, ssm);
			ESP_LOGI(TAG, "There are 2 Sesame devices");
			ESP_LOGI(TAG, "Automatically add %s to %s", SSM_PRODUCT_TYPE_STR(ssm->product_type), SSM_PRODUCT_TYPE_STR(tch->product_type));
			if (tch == ssm1) {
				disconnect(tch);
			}
		}
	}
}
