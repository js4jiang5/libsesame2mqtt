#include "ssm_cmd.h"
#include "aes-cbc-cmac.h"
#include "blecent.h"
#include "esp_log.h"
#include "esp_random.h"
#include "uECC.h"
#include <string.h>

static const char * TAG = "ssm_cmd.c";
static uint8_t tag_esp32[] = { 'S', 'E', 'S', 'A', 'M', 'E', ' ', 'E', 'S', 'P', '3', '2' };
static uint8_t ecc_private_esp32[32];

static int crypto_backend_micro_ecc_rng_callback(uint8_t * dest, unsigned size) {
    esp_fill_random(dest, (size_t) size);
    return 1;
}

void send_reg_cmd_to_ssm(sesame * ssm) {
    ESP_LOGW(TAG, "[esp32->%s][register]", SSM_PRODUCT_TYPE_STR(ssm->product_type));
    uECC_set_rng(crypto_backend_micro_ecc_rng_callback);
    uint8_t ecc_public_esp32[64];
    uECC_make_key_lit(ecc_public_esp32, ecc_private_esp32, uECC_secp256r1());
    ssm->c_offset = sizeof(ecc_public_esp32) + 1;
    ssm->b_buf[0] = SSM_ITEM_CODE_REGISTRATION;
    memcpy(ssm->b_buf + 1, ecc_public_esp32, sizeof(ecc_public_esp32));
    talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_PLAINTEXT);
}

void handle_reg_data_from_ssm(sesame * ssm) {
    ESP_LOGW(TAG, "[esp32<-%s][register]", SSM_PRODUCT_TYPE_STR(ssm->product_type));
    if (ssm->product_type == SESAME_5 || ssm->product_type == SESAME_5_PRO) { // Sesame5 or Sesame5 Pro Lock
        memcpy(ssm->public_key, &ssm->b_buf[13], 64);
    } else { // Sesame Touch
        memcpy(ssm->public_key, &ssm->b_buf[0], 64);
    }
    uint8_t ecdh_secret_ssm[32];
    uECC_shared_secret_lit(ssm->public_key, ecc_private_esp32, ecdh_secret_ssm, uECC_secp256r1());
    memcpy(ssm->device_secret, ecdh_secret_ssm, 16);
    // ESP_LOG_BUFFER_HEX("deviceSecret", ssm->device_secret, 16);
    AES_CMAC(ssm->device_secret, (const unsigned char *) ssm->cipher.decrypt.random_code, 4, ssm->cipher.token);
    ssm->device_status = SSM_LOGGIN;
    ssm_save_nvs(ssm); // save ssm configurations in NVS
    ESP_LOGI(TAG, "%s NVS save done", SSM_PRODUCT_TYPE_STR(ssm->product_type));

    ssm->is_new = 1;
    cnt_unregistered_ssms++; // update number of unregistered devices
    send_login_cmd_to_ssm(ssm);
}

void send_login_cmd_to_ssm(sesame * ssm) {
    ESP_LOGW(TAG, "[esp32->%s][login]", SSM_PRODUCT_TYPE_STR(ssm->product_type));
    ssm->b_buf[0] = SSM_ITEM_CODE_LOGIN;
    AES_CMAC(ssm->device_secret, (const unsigned char *) ssm->cipher.decrypt.random_code, 4, ssm->cipher.token);
    memcpy(&ssm->b_buf[1], ssm->cipher.token, 4);
    ssm->c_offset = 5;
    talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_PLAINTEXT);

    // one more registered device login successfully
    if (ssm->cnt_discovery == 0) {
        ssm->id = cnt_ssms;
        ssm->cnt_discovery++;
        cnt_ssms++; // found one more controllable SSM device
        ESP_LOGW(TAG, "cnt_ssms = %d, cnt_unregistered_ssms = %d", cnt_ssms, cnt_unregistered_ssms);
    }
    ssm->wait_for_status_update_from_ssm = 1; // start waiting for status update
    if (!sesame_search_done()) {              // if search is not done yet, restart BLE scan
        ble_hs_cfg.sync_cb();                 // bls_hs_cfg.sync_cb callback function is blecent_scan, which invoke ble_gap_disc()
    }
}

void send_read_history_cmd_to_ssm(sesame * ssm) {
    ESP_LOGI(TAG, "[send_read_history_cmd_to_%s]", SSM_PRODUCT_TYPE_STR(ssm->product_type));
    ssm->c_offset = 2;
    ssm->b_buf[0] = SSM_ITEM_CODE_HISTORY;
    ssm->b_buf[1] = 1;
    talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
}

void ssm_lock(sesame * ssm, uint8_t * tag, uint8_t tag_length) {
    if (ssm->device_status >= SSM_LOGGIN) {
        if (tag_length == 0) {
            tag = tag_esp32;
            tag_length = sizeof(tag_esp32);
        }
        ssm->b_buf[0] = SSM_ITEM_CODE_LOCK;
        ssm->b_buf[1] = tag_length;
        ssm->c_offset = tag_length + 2;
        memcpy(ssm->b_buf + 2, tag, tag_length);
        talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void ssm_unlock(sesame * ssm, uint8_t * tag, uint8_t tag_length) {
    if (ssm->device_status >= SSM_LOGGIN) {
        if (tag_length == 0) {
            tag = tag_esp32;
            tag_length = sizeof(tag_esp32);
        }
        ssm->b_buf[0] = SSM_ITEM_CODE_UNLOCK;
        ssm->b_buf[1] = tag_length;
        ssm->c_offset = tag_length + 2;
        memcpy(ssm->b_buf + 2, tag, tag_length);
        talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void ssm_magnet(sesame * ssm) {
    if (ssm->device_status >= SSM_LOGGIN) {
        ssm->b_buf[0] = SSM_ITEM_CODE_MAGNET;
        ssm->c_offset = 1;
        talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void ssm_mech(sesame * ssm, int lock_position, int unlock_position) {
    if (ssm->device_status >= SSM_LOGGIN) {
        ssm->mech.lock_unlock.lock = lock_position;
        ssm->mech.lock_unlock.unlock = unlock_position;
        ssm->mech.auto_lock_second = 0;
        ssm->b_buf[0] = SSM_ITEM_CODE_MECH_SETTING;
        ssm->c_offset = sizeof(ssm->mech) + 1;
        memcpy(ssm->b_buf + 1, &ssm->mech, sizeof(ssm->mech));
        talk_to_ssm(ssm, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_add_sesame(sesame * tch, sesame * ssm) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_ADD_SESAME;
        tch->c_offset = 1 + 16 + 16;
        memcpy(tch->b_buf + 1, ssm->device_uuid, sizeof(ssm->device_uuid));
        memcpy(tch->b_buf + 1 + 16, ssm->device_secret, sizeof(ssm->device_secret));
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_remove_sesame(sesame * tch, sesame * ssm) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_REMOVE_SESAME;
        tch->c_offset = 1 + 16;
        memcpy(tch->b_buf + 1, ssm->device_uuid, sizeof(ssm->device_uuid));
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_finger_add(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_FINGER_MODE_SET;
        tch->b_buf[1] = 1; // set to add mode
        tch->c_offset = 2;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_finger_verify(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_FINGER_MODE_SET;
        tch->b_buf[1] = 2; // set to verify mode
        tch->c_offset = 2;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_card_add(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_CARD_MODE_SET;
        tch->b_buf[1] = 1; // set to add mode
        tch->c_offset = 2;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_card_verify(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_CARD_MODE_SET;
        tch->b_buf[1] = 0; // set to verify mode
        tch->c_offset = 2;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_finger_mode_get(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_FINGER_MODE_GET;
        tch->c_offset = 1;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_card_mode_get(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_CARD_MODE_GET;
        tch->c_offset = 1;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_finger_get(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_FINGER_GET;
        tch->c_offset = 1;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}

void tch_card_get(sesame * tch) {
    if (sesame_search_done()) {
        tch->b_buf[0] = SSM_ITEM_CODE_CARD_GET;
        tch->c_offset = 1;
        talk_to_ssm(tch, SSM_SEG_PARSING_TYPE_CIPHERTEXT);
    }
}
