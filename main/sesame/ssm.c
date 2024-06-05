#include "ssm.h"
#include "blecent.h"
#include "c_ccm.h"
#include "esp_central.h"
#include "mqtt_section.h"
#include "nvs_flash.h"
#include "ssm_cmd.h"

static const char * TAG = "ssm.c";

static uint8_t additional_data[] = { 0x00 };

static double battery_vol[] = {5.85, 5.82, 5.79, 5.76, 5.73, 5.70, 5.65, 5.60, 5.55, 5.50, 5.40, 5.20, 5.10, 5.0, 4.8, 4.6}; // 20240526 by JS

static double battery_pct[] = {100.0, 95.0, 90.0, 85.0, 80.0, 70.0, 60.0, 50.0, 40.0, 32.0, 21.0, 13.0, 10.0, 7.0, 3.0, 0.0}; // 20240526 by JS

uint8_t cnt_ssms = 0, cnt_unregistered_ssms = 0, real_num_ssms = 0;

struct ssm_env_tag * p_ssms_env = NULL;

struct timeval tv_start, tv_1min;

int hex2dec(char hex_letter) {
	int v = 0;
	switch (hex_letter) {
	case '0':
		v = 0;
		break;
	case '1':
		v = 1;
		break;
	case '2':
		v = 2;
		break;
	case '3':
		v = 3;
		break;
	case '4':
		v = 4;
		break;
	case '5':
		v = 5;
		break;
	case '6':
		v = 6;
		break;
	case '7':
		v = 7;
		break;
	case '8':
		v = 8;
		break;
	case '9':
		v = 9;
		break;
	case 'A':
		v = 10;
		break;
	case 'B':
		v = 11;
		break;
	case 'C':
		v = 12;
		break;
	case 'D':
		v = 13;
		break;
	case 'E':
		v = 14;
		break;
	case 'F':
		v = 15;
		break;
	case 'a':
		v = 10;
		break;
	case 'b':
		v = 11;
		break;
	case 'c':
		v = 12;
		break;
	case 'd':
		v = 13;
		break;
	case 'e':
		v = 14;
		break;
	case 'f':
		v = 15;
		break;
	default:
		break;
	}
	return v;
}

void gen_qr_code_txt(sesame * ssm, char * qr) {
	static const unsigned char base64_table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int cnt = 0;
	char key[80] = {};

	if (ssm->product_type == SESAME_5) { // 2 chars of product type
		cnt += sprintf(key + cnt, "05");
	} else if (ssm->product_type == SESAME_5_PRO) {
		cnt += sprintf(key + cnt, "07");
	} else if (ssm->product_type == SESAME_TOUCH_PRO) {
		cnt += sprintf(key + cnt, "09");
	} else if (ssm->product_type == SESAME_TOUCH) {
		cnt += sprintf(key + cnt, "0a");
	}

	for (int n = 0; n < 16; n++) { // 32 chars of secret key (16 bytes)
		cnt += sprintf(key + cnt, "%02x", ssm->device_secret[n]);
	}

	for (int n = 0; n < 4; n++) { // 8 chars of public key (4 bytes)
		cnt += sprintf(key + cnt, "%02x", ssm->public_key[n]);
	}
	cnt += sprintf(key + cnt, "0000"); // 4 zeros
	for (int n = 0; n < 16; n++) {	   // 32 chars of uuid key (16 bytes)
		cnt += sprintf(key + cnt, "%02x", ssm->device_uuid[n]);
	}
	ESP_LOGI(TAG, "key = %s, cnt = %d", key, cnt);
	// totally 2 + 32 + 8 + 4 + 32 = 78 chars, is 3 x 26 composed of 2x26 = 52 base64 characters
	// convert to base64
	cnt = 0;
	cnt += sprintf(qr + cnt, "ssm://UI?t=sk&sk="); // prefix
	for (int n = 0; n < 78; n += 3) {			   // 78/3*2 = 52 base64 chars
		int v = (hex2dec(key[n]) << 8) + (hex2dec(key[n + 1]) << 4) + hex2dec(key[n + 2]);
		cnt += sprintf(qr + cnt, "%c%c", base64_table[v >> 6], base64_table[v & 0x3F]);
	}
	cnt += sprintf(qr + cnt, "&l=0&n=");
	if (ssm->product_type == SESAME_5) { // URL of name
		cnt += sprintf(qr + cnt, "%%E8%%8A%%9D%%E9%%BA%%BB5");
	} else if (ssm->product_type == SESAME_5_PRO) {
		cnt += sprintf(qr + cnt, "%%E8%%8A%%9D%%E9%%BA%%BB5%20Pro");
	} else if (ssm->product_type == SESAME_TOUCH_PRO) {
		cnt += sprintf(qr + cnt, "Sesame%%20Touch%%20PRO");
	} else if (ssm->product_type == SESAME_TOUCH) {
		cnt += sprintf(qr + cnt, "Sesame%%20Touch");
	}
}

int timer_1min(void) {
	struct timeval tv_now;
	gettimeofday(&tv_now, NULL); // get current time
	int diff = tv_now.tv_sec - tv_1min.tv_sec;
	if (diff >= 60) {
		gettimeofday(&tv_1min, NULL); // restart timer
		return 1;
	} else {
		return 0;
	}
}

void start_timer(void) {
	gettimeofday(&tv_start, NULL); // loop start timer
}

int loop_timeout(void) {
	struct timeval tv_now;
	gettimeofday(&tv_now, NULL); // get current time
	int diff = tv_now.tv_sec - tv_start.tv_sec;
	return ((diff > CONFIG_ESP_TASK_WDT_TIMEOUT_S - 3 || diff < 0)); // timeout 3 secs later or time variable wrap around
}

int wait_for_status_update(sesame * ssm, uint8_t timeout_s) {
	uint8_t n_max = timeout_s * 10;
	int succeed = 0;
	for (int n = 0; n < n_max; n++) {
		if (ssm->update_status) {
			ESP_LOGI(TAG, "%s received status update", SSM_PRODUCT_TYPE_STR(ssm->product_type));
			succeed = 1;
			break;
		} else if (n == n_max - 1) { // timeout
			ESP_LOGW(TAG, "%s wait status timeout", SSM_PRODUCT_TYPE_STR(ssm->product_type));
		} else {
			vTaskDelay(100 / portTICK_PERIOD_MS);
		}
	}
	return succeed;
}

int ssm_read_nvs(sesame * ssm) {
	nvs_handle_t my_handle;
	esp_err_t err;
	size_t len = 0;
	uint8_t found = 0;
	uint8_t addr[6];

	// generate topic from MAC address as NVS name. The format is s2mooxxooxxooxx
	memset(ssm->topic, 0, sizeof(ssm->topic));
	int cnt = 0;
	cnt += sprintf(ssm->topic + cnt, "s2m");
	for (int n = 0; n < 6; n++) {
		cnt += sprintf(ssm->topic + cnt, "%02x", ssm->addr[n]);
	}

	err = nvs_open(ssm->topic, NVS_READONLY, &my_handle);
	if (err == ESP_OK) {
		len = sizeof(ssm->addr);
		err = nvs_get_blob(my_handle, "addr", addr, &len);
		if (err == ESP_OK && memcmp(addr, ssm->addr, 6) == 0) { // double check if the address is the same
			found = 1;
			len = sizeof(ssm->device_uuid);
			err = nvs_get_blob(my_handle, "device_uuid", ssm->device_uuid, &len);
			len = sizeof(ssm->public_key);
			err = nvs_get_blob(my_handle, "public_key", ssm->public_key, &len);
			len = sizeof(ssm->device_secret);
			err = nvs_get_blob(my_handle, "device_secret", ssm->device_secret, &len);
			len = sizeof(ssm->cipher);
			err = nvs_get_blob(my_handle, "cipher", (void *) (&ssm->cipher), &len);
			len = sizeof(ssm->mech_status);
			err = nvs_get_blob(my_handle, "mech_status", (void *) (&ssm->mech_status), &len);
			len = sizeof(ssm->topic);
			err = nvs_get_u16(my_handle, "c_offset", &ssm->c_offset);
			err = nvs_get_u8(my_handle, "conn_id", &ssm->conn_id);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "NVS read error");
			}
		}
	}
	// NVS close
	nvs_close(my_handle);

	if (found) {
		ESP_LOGI(TAG, "NVS read done");
	} else {
		ESP_LOGW(TAG, "NVS read failed");
	}
	return found;
}

int ssm_save_nvs(sesame * ssm) {
	nvs_handle_t my_handle;
	esp_err_t err;
	int save_done = 0;

	// generate topic from MAC address as NVS name. The format is s2mooxxooxxooxx
	memset(ssm->topic, 0, sizeof(ssm->topic));
	int cnt = 0;
	cnt += sprintf(ssm->topic + cnt, "s2m");
	for (int n = 0; n < 6; n++) {
		cnt += sprintf(ssm->topic + cnt, "%02x", ssm->addr[n]);
	}
	err = nvs_open(ssm->topic, NVS_READWRITE, &my_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "NVS OPEN error");
	} else {
		// NVS write
		err = nvs_set_blob(my_handle, "device_uuid", ssm->device_uuid, sizeof(ssm->device_uuid));
		err = nvs_set_blob(my_handle, "public_key", ssm->public_key, sizeof(ssm->public_key));
		err = nvs_set_blob(my_handle, "device_secret", ssm->device_secret, sizeof(ssm->device_secret));
		err = nvs_set_blob(my_handle, "addr", ssm->addr, sizeof(ssm->addr));
		err = nvs_set_blob(my_handle, "cipher", (const void *) (&ssm->cipher), sizeof(ssm->cipher));
		err = nvs_set_blob(my_handle, "mech_status", (const void *) (&ssm->mech_status), sizeof(ssm->mech_status));
		err = nvs_set_u16(my_handle, "c_offset", ssm->c_offset);
		err = nvs_set_u8(my_handle, "conn_id", ssm->conn_id);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "NVS write error");
		} else {
			// NVS commit
			err = nvs_commit(my_handle);
			if (err == ESP_OK) {
				save_done = 1;
			} else {
				ESP_LOGE(TAG, "NVS commit error");
			}
		}
	}
	// NVS close
	nvs_close(my_handle);

	if (save_done) {
		ESP_LOGI(TAG, "NVS save done");
	} else {
		ESP_LOGW(TAG, "NVS save failed");
	}
	return save_done;
}

static void ssm_initial_handle(sesame * ssm, uint8_t cmd_it_code) {
	ssm->cipher.encrypt.nouse = 0; // reset cipher
	ssm->cipher.decrypt.nouse = 0;
	memcpy(ssm->cipher.encrypt.random_code, ssm->b_buf, 4);
	memcpy(ssm->cipher.decrypt.random_code, ssm->b_buf, 4);
	ssm->cipher.encrypt.count = 0;
	ssm->cipher.decrypt.count = 0;
	if (ssm->device_secret[0] == 0) {
		ESP_LOGI(TAG, "[ssm][no device_secret]");
		send_reg_cmd_to_ssm(ssm);
		return;
	}
	send_login_cmd_to_ssm(ssm);
}

static void ssm_parse_publish(sesame * ssm, uint8_t cmd_it_code) {
	switch (cmd_it_code) {
	case SSM_ITEM_CODE_INITIAL: // get 4 bytes random_code
		ssm_initial_handle(ssm, cmd_it_code);
		break;
	case SSM_ITEM_CODE_MECH_STATUS:
		memcpy((void *) &(ssm->mech_status), ssm->b_buf, 7);
		ESP_LOGI(TAG, "%s:", SSM_PRODUCT_TYPE_STR(ssm->product_type));
		ESP_LOGI(TAG, "battery = %d", ssm->mech_status.battery);
		ESP_LOGI(TAG, "target = %d", ssm->mech_status.target);
		ESP_LOGI(TAG, "position = %d", ssm->mech_status.position);
		ESP_LOGI(TAG, "is_clutch_failed = %d", ssm->mech_status.is_clutch_failed);
		ESP_LOGI(TAG, "is_lock_range = %d", ssm->mech_status.is_lock_range);
		ESP_LOGI(TAG, "is_unlock_range = %d", ssm->mech_status.is_unlock_range);
		ESP_LOGI(TAG, "is_critical = %d", ssm->mech_status.is_critical);
		ESP_LOGI(TAG, "is_stop = %d", ssm->mech_status.is_stop);
		ESP_LOGI(TAG, "is_low_battery = %d", ssm->mech_status.is_low_battery);
		ESP_LOGI(TAG, "is_clockwise = %d", ssm->mech_status.is_clockwise);
		device_status_t lockStatus = ssm->mech_status.is_lock_range ? SSM_LOCKED : (ssm->mech_status.is_unlock_range ? SSM_UNLOCKED : SSM_MOVED);
		ssm->device_status = lockStatus;
		ssm->update_status = 1;

		// calculate battery percentage from voltage
		ssm->battery_percentage = 0.;
		double voltage = ssm->mech_status.battery * 2/1000.;
		for (int i_bat = 0; i_bat < 16; i_bat++) {
			if (voltage > battery_vol[i_bat]) {
				ssm->battery_percentage = battery_pct[i_bat];
				break;
			}
		}

		p_ssms_env->ssm_cb__(ssm); // callback: ssm_action_handle
		break;
	default:
		break;
	}
}

static void ssm_parse_response(sesame * ssm, uint8_t cmd_it_code) {
	switch (cmd_it_code) {
	case SSM_ITEM_CODE_REGISTRATION:
		ssm->c_offset = ssm->c_offset - 1; // c_offset minus 1 is only required for registration. I think this is a workaround for their bug. 2024.04.18 by JS
		memcpy(ssm->b_buf, ssm->b_buf + 1, ssm->c_offset);
		handle_reg_data_from_ssm(ssm);
		break;
	case SSM_ITEM_CODE_LOGIN:
		ESP_LOGI(TAG, "[%d][%s][login][ok]", ssm->conn_id, SSM_PRODUCT_TYPE_STR(ssm->product_type));
		ssm->device_status = SSM_LOGGIN;
		break;
	case SSM_ITEM_CODE_HISTORY:
		ESP_LOGI(TAG, "[%d][%s][hisdataLength: %d]", ssm->conn_id, SSM_PRODUCT_TYPE_STR(ssm->product_type), ssm->c_offset);
		if (ssm->c_offset == 0) { // 循環讀取 避免沒取完歷史
			return;
		}
		send_read_history_cmd_to_ssm(ssm);
		break;
	case SSM_ITEM_CODE_FINGER_MODE_SET:
		ESP_LOGI(TAG, "Finger mode set %s, Finger mode is %s", (ssm->b_buf[0] == 0) ? "success" : "fail", (ssm->b_buf[1] == 0) ? "verify" : "add");
		break;
	case SSM_ITEM_CODE_FINGER_MODE_GET:
		ESP_LOGI(TAG, "Finger mode get %s, Finger mode is %s", (ssm->b_buf[0] == 0) ? "success" : "fail", (ssm->b_buf[1] == 0) ? "verify" : "add");
		break;
	case SSM_ITEM_CODE_FINGER_GET:
		ESP_LOGI(TAG, "Finger get %s", (ssm->b_buf[0] == 0) ? "success" : "fail");
		break;
	case SSM_ITEM_CODE_CARD_MODE_SET:
		ESP_LOGI(TAG, "Card mode set %s, Card mode is %s", (ssm->b_buf[0] == 0) ? "success" : "fail", (ssm->b_buf[1] == 0) ? "verify" : "add");
		break;
	case SSM_ITEM_CODE_CARD_MODE_GET:
		ESP_LOGI(TAG, "Card mode get %s, Card mode is %s", (ssm->b_buf[0] == 0) ? "success" : "fail", (ssm->b_buf[1] == 0) ? "verify" : "add");
		break;
	case SSM_ITEM_CODE_CARD_GET:
		ESP_LOGI(TAG, "Card get %s", (ssm->b_buf[0] == 0) ? "success" : "fail");
		break;
	default:
		break;
	}
}

void ssm_ble_receiver(sesame * ssm, const uint8_t * p_data, uint16_t len) {
	ssm->update_status = 0;
	if (p_data[0] & 1u) {
		ssm->c_offset = 0;
	}
	memcpy(&ssm->b_buf[ssm->c_offset], p_data + 1, len - 1);
	ssm->c_offset += len - 1;
	if (p_data[0] >> 1u == SSM_SEG_PARSING_TYPE_APPEND_ONLY) {
		return;
	}
	if (p_data[0] >> 1u == SSM_SEG_PARSING_TYPE_CIPHERTEXT) {
		ssm->c_offset = ssm->c_offset - CCM_TAG_LENGTH;
		aes_ccm_auth_decrypt(ssm->cipher.token, (const unsigned char *) &ssm->cipher.decrypt, 13, additional_data, 1, ssm->b_buf, ssm->c_offset, ssm->b_buf, ssm->b_buf + ssm->c_offset, CCM_TAG_LENGTH);
		ssm->cipher.decrypt.count++;
	}

	uint8_t cmd_op_code = ssm->b_buf[0];
	uint8_t cmd_it_code = ssm->b_buf[1];
	ssm->c_offset = ssm->c_offset - 2;
	memcpy(ssm->b_buf, ssm->b_buf + 2, ssm->c_offset);
	ESP_LOGI(TAG, "[%s][say][%d][%s][%s]", SSM_PRODUCT_TYPE_STR(ssm->product_type), ssm->conn_id, SSM_OP_CODE_STR(cmd_op_code), SSM_ITEM_CODE_STR(cmd_it_code));

	if (cmd_op_code == SSM_OP_CODE_PUBLISH) {
		ssm_parse_publish(ssm, cmd_it_code);
	} else if (cmd_op_code == SSM_OP_CODE_RESPONSE) {
		ssm_parse_response(ssm, cmd_it_code);
	}
	ssm->c_offset = 0;
}

void talk_to_ssm(sesame * ssm, uint8_t parsing_type) {
	ESP_LOGI(TAG, "[esp32][say][%d][%s]", ssm->conn_id, SSM_ITEM_CODE_STR(ssm->b_buf[0]));
	if (parsing_type == SSM_SEG_PARSING_TYPE_CIPHERTEXT) {
		aes_ccm_encrypt_and_tag(ssm->cipher.token, (const unsigned char *) &ssm->cipher.encrypt, 13, additional_data, 1, ssm->b_buf, ssm->c_offset, ssm->b_buf, ssm->b_buf + ssm->c_offset, CCM_TAG_LENGTH);
		ssm->cipher.encrypt.count++;
		ssm->c_offset = ssm->c_offset + CCM_TAG_LENGTH;
	}

	uint8_t * data = ssm->b_buf;
	uint16_t remain = ssm->c_offset;
	uint16_t len = remain;
	uint8_t tmp_v[20] = { 0 };
	uint16_t len_l;

	while (remain) {
		if (remain <= 19) {
			tmp_v[0] = parsing_type << 1u;
			len_l = 1 + remain;
		} else {
			tmp_v[0] = 0;
			len_l = 20;
		}
		if (remain == len) {
			tmp_v[0] |= 1u;
		}
		memcpy(&tmp_v[1], data, len_l - 1);
		esp_ble_gatt_write(ssm, tmp_v, len_l);
		remain -= (len_l - 1);
		data += (len_l - 1);
	}
}

void ssm_mem_deinit(void) {
	free(p_ssms_env);
}

void ssm_init(ssm_action ssm_action_cb) {
	cnt_ssms = 0;
	cnt_unregistered_ssms = 0;
	p_ssms_env = (struct ssm_env_tag *) calloc(SSM_MAX_NUM, sizeof(struct ssm_env_tag));
	if (p_ssms_env == NULL) {
		ESP_LOGE(TAG, "[ssms_init][FAIL]");
	}
	for (int n = 0; n < SSM_MAX_NUM; n++) {
		(p_ssms_env + n)->ssm_cb__ = ssm_action_cb; // callback: ssm_action_handle
		(p_ssms_env + n)->ssm.conn_id = 0xFF;		// 0xFF: not connected
		(p_ssms_env + n)->ssm.id = 0xFF;			// 0xFF: address offset is not concluded yet
		(p_ssms_env + n)->ssm.device_status = SSM_NOUSE;
		(p_ssms_env + n)->ssm.mech.lock_unlock.lock = 160;		   // 20240508 add by JS
		(p_ssms_env + n)->ssm.mech.lock_unlock.unlock = 20;		   // 20240508 add by JS
		(p_ssms_env + n)->ssm.mech.auto_lock_second = 0;		   // 20240508 add by JS
		(p_ssms_env + n)->ssm.add_card = 0;						   // 20240508 add by JS
		(p_ssms_env + n)->ssm.add_finger = 0;					   // 20240508 add by JS
		(p_ssms_env + n)->ssm.cnt_discovery = 0;				   // 20240510 add by JS
		(p_ssms_env + n)->ssm.is_new = 0;						   // 20240516 add by JS
		(p_ssms_env + n)->ssm.mqtt_discovery_done = 0;			   // 20240524 add by JS
		(p_ssms_env + n)->ssm.mqtt_subscribe_done = 0;			   // 20240524 add by JS
		(p_ssms_env + n)->ssm.disconnect_forever = 0;			   // 20240605 add by JS
		(p_ssms_env + n)->ssm.update_status = 0;				   // 20240605 add by JS
		(p_ssms_env + n)->ssm.rssi = 0;			   				   // 20240605 add by JS
		(p_ssms_env + n)->ssm.is_alive = 0;						   // 20240605 add by JS
		(p_ssms_env + n)->ssm.rssi_changed = 0;					   // 20240605 add by JS
		memset((p_ssms_env + n)->ssm.topic, 0, sizeof((p_ssms_env + n)->ssm.topic));
	}
	ESP_LOGI(TAG, "[ssms_init][SUCCESS]");
}