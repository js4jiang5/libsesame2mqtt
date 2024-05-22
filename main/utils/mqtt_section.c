/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "blecent.h"
#include "esp_central.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt_section.h"
#include "ssm_cmd.h"

static const char * TAG = "mqtt_section.c";

esp_mqtt_client_handle_t client_ssm;
int mqtt_init_done = 0;
int mqtt_discovery_done = 0;
int msg_id_subscribed = 0;
char config_broker_url[60] = {};
uint8_t cnt_HA_devices = 0;

static void log_error_if_nonzero(const char * message, int error_code) {
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

double get_json_value(char * data, int * quote_index, int id1, int id2) {
    char * ptr = data + quote_index[id1] + 1;
    int len = quote_index[id2] - quote_index[id1] - 1;
    char str[30];
    strncpy(str, ptr, len);
    return atof(str);
}

void get_json_str(char * data, int * quote_index, int id1, int id2, char * str) {
    char * ptr = data + quote_index[id1] + 1;
    int len = quote_index[id2] - quote_index[id1] - 1;
    strncpy(str, ptr, len);
    for (; *str; ++str)
        *str = tolower(*str);
}

int wait_published(int msg_id) {
    int n_max = 100; // wait at most 10 secs
    int subscribed = 0;
    for (int n = 0; n < n_max; n++) {
        if (msg_id == msg_id_subscribed) {
            subscribed = 1;
            break;
        } else {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
    if (subscribed) {
        ESP_LOGI(TAG, "published msg_id = %d", msg_id);
        return 1;
    } else {
        ESP_LOGW(TAG, "publish failed, msg_id = %d", msg_id);
        return 0;
    }
}

int wake_up(sesame * ssm) {
    int succeed = 0;
    for (int n = 0; n < 5; n++) { // try at most 5 times
        // disconnect and reconnect to login and update status
        disconnect(ssm);
        vTaskDelay(600 / portTICK_PERIOD_MS);
        ssm->wait_for_status_update_from_ssm = 1;
        reconnect(ssm);
        if (wait_for_status_update(ssm, 20)) {
            succeed = 1;
            break;
        }
    }
    return succeed;
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void * handler_args, esp_event_base_t base, int32_t event_id, void * event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    // esp_mqtt_client_handle_t client = event->client;
    // int msg_id = 0;
    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        // msg_id = esp_mqtt_client_subscribe(client, "HA-SesameLock/set", 2); // QOS 2
        // ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        if (mqtt_discovery_done) { // if MQTT broker disconnect and reconnect, reboot ESP. Currently I don't know a better way to solve it without reboot 20240522 by JS
            esp_restart();
        }
        mqtt_init_done = 1;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        // msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        // ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        msg_id_subscribed = event->msg_id;
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        printf("QoS= %d, retain=%d\r\n", event->qos, event->retain);

        if (!mqtt_discovery_done) {
            return;
        }

        // find ssm
        sesame *ssm = NULL, *tch = NULL;
        char topic[80] = "";
        char empty_payload[2] = "";
        uint8_t valid = 0;
        for (int n = 0; n < cnt_ssms; n++) {
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s/set", (p_ssms_env + n)->ssm.topic); // command topic
            ESP_LOGI(TAG, "topic = %s", topic);
            ESP_LOGI(TAG, "event->topic = %s", event->topic);
            if (strncmp(event->topic, topic, strlen(topic)) == 0) {
                valid = 1;
                if ((p_ssms_env + n)->ssm.product_type == SESAME_TOUCH || (p_ssms_env + n)->ssm.product_type == SESAME_TOUCH_PRO) {
                    tch = &(p_ssms_env + n)->ssm;
                } else {
                    ssm = &(p_ssms_env + n)->ssm;
                }
                break;
            }
        }

        if (valid) {
            int quote_index[16] = { 0 };
            int i_quote = 0;
            char * action;
            int action_len = 0;

            for (int n = 0; n < event->data_len; n++) {
                if (event->data[n] == '"') {
                    quote_index[i_quote++] = n;
                }
            }

            if (i_quote == 0) {
                action = event->data;
                action_len = event->data_len;
            } else {
                action = event->data + quote_index[2] + 1;
                action_len = quote_index[3] - quote_index[2] - 1;
            }
            ESP_LOGI(TAG, "action_len = %d", action_len);

            if (action_len == strlen("lock") && strncmp(action, "lock", strlen("lock")) == 0) {
                ssm_lock(ssm, NULL, 0);
                return;
            }
            if (action_len == strlen("unlock") && strncmp(action, "unlock", strlen("unlock")) == 0) {
                ssm_unlock(ssm, NULL, 0);
                return;
            }
            if (action_len == strlen("magnet") && strncmp(action, "magnet", strlen("magnet")) == 0) {
                ssm_magnet(ssm);
                return;
            }
            if (action_len == strlen("gen_qr_code_text") && strncmp(action, "gen_qr_code_text", strlen("gen_qr_code_text")) == 0) {
                int msg_id;
                (ssm == NULL) ? (ssm = tch) : (tch = ssm);
                memset(topic, 0, sizeof(topic));
                sprintf(topic, "homeassistant/%s/state/qr_code_text", ssm->topic); // config topic
                char qr[120] = "";
                gen_qr_code_txt(ssm, qr);
                msg_id = esp_mqtt_client_publish(client_ssm, topic, qr, 0, 2, 0); // QOS 2, retain 1
                ESP_LOGI(TAG, "sent mqtt qr code text for %s, msg_id=%d", ssm->topic, msg_id);
            }
            if (action_len == strlen("add_sesame") && strncmp(action, "add_sesame", strlen("add_sesame")) == 0) {
                int found = 0;
                char addr[20] = {};
                get_json_str(event->data, quote_index, 6, 7, addr);
                ESP_LOGI(TAG, "Request to add sesame with mac %s", addr);
                for (int n = 0; n < cnt_ssms; n++) {
                    memset(topic, 0, sizeof(topic));
                    ESP_LOGI(TAG, "addr = %s", addr_str((p_ssms_env + n)->ssm.addr));
                    if (strncmp(addr, addr_str((p_ssms_env + n)->ssm.addr), 17) == 0) {
                        found = 1;
                        ssm = &(p_ssms_env + n)->ssm;
                        ESP_LOGI(TAG, "Sesame with mac %s is found", addr);
                    }
                }

                if (found) {
                    if (wake_up(tch)) {
                        tch_add_sesame(tch, ssm);
                        ESP_LOGI(TAG, "Add sesame with mac = %s", addr);
                        vTaskDelay(200 / portTICK_PERIOD_MS);
                        // int msg_id = 0;
                        // memset(topic, 0, sizeof(topic));
                        // sprintf(topic, "homeassistant/%s/state/add_sesame", ssm->topic); // config topic
                        // msg_id = esp_mqtt_client_publish(client_ssm, topic, empty_payload, 0, 2, 0); // QOS 2, retain 1
                        // ESP_LOGI(TAG, "sent \"\" for add_sesame for %s, msg_id=%d", ssm->topic, msg_id);
                        // wait_published(msg_id);
                    }
                }
                disconnect(tch);
                return;
            }
            if (action_len == strlen("remove_sesame") && strncmp(action, "remove_sesame", strlen("remove_sesame")) == 0) {
                int found = 0;
                char addr[20] = {};
                get_json_str(event->data, quote_index, 6, 7, addr);
                ESP_LOGI(TAG, "Request to remove sesame with mac %s", addr);
                for (int n = 0; n < cnt_ssms; n++) {
                    memset(topic, 0, sizeof(topic));
                    ESP_LOGI(TAG, "addr = %s", addr_str((p_ssms_env + n)->ssm.addr));
                    if (strncmp(addr, addr_str((p_ssms_env + n)->ssm.addr), 17) == 0) {
                        found = 1;
                        ssm = &(p_ssms_env + n)->ssm;
                        ESP_LOGI(TAG, "Sesame with mac %s is found", addr);
                    }
                }

                if (found) {
                    if (wake_up(tch)) {
                        tch_remove_sesame(tch, ssm);
                        ESP_LOGI(TAG, "Remove sesame with mac = %s", addr);
                        vTaskDelay(200 / portTICK_PERIOD_MS);
                        // int msg_id = 0;
                        // memset(topic, 0, sizeof(topic));
                        // sprintf(topic, "homeassistant/%s/state/remove_sesame", ssm->topic); // config topic
                        // msg_id = esp_mqtt_client_publish(client_ssm, topic, empty_payload, 0, 2, 0); // QOS 2, retain 1
                        // ESP_LOGI(TAG, "sent \"\" for remove_sesame for %s, msg_id=%d", ssm->topic, msg_id);
                        // wait_published(msg_id);
                    }
                }
                disconnect(tch);
                return;
            }
            if (action_len == strlen("add_finger") && strncmp(action, "add_finger", strlen("add_finger")) == 0) {
                ESP_LOGI(TAG, "change to add finger mode");
                if (wake_up(tch)) {
                    tch->add_finger = 1;
                    tch_finger_add(tch);
                    ESP_LOGI(TAG, "start add finger mode");
                }
                return;
            }
            if (action_len == strlen("verify_finger") && strncmp(action, "verify_finger", strlen("verify_finger")) == 0) {
                ESP_LOGI(TAG, "change to verify finger mode");
                if (wake_up(tch)) {
                    tch->add_finger = 0;
                    tch_finger_verify(tch);
                    vTaskDelay(200 / portTICK_PERIOD_MS);
                }
                disconnect(tch);
                return;
            }
            if (action_len == strlen("add_card") && strncmp(action, "add_card", strlen("add_card")) == 0) {
                ESP_LOGI(TAG, "change to add card mode");
                if (wake_up(tch)) {
                    tch->add_card = 1;
                    tch_card_add(tch);
                    ESP_LOGI(TAG, "start add card mode");
                }
                return;
            }
            if (action_len == strlen("verify_card") && strncmp(action, "verify_card", strlen("verify_card")) == 0) {
                ESP_LOGI(TAG, "change to verify card mode");
                if (wake_up(tch)) {
                    tch->add_card = 0;
                    tch_card_verify(tch);
                    vTaskDelay(200 / portTICK_PERIOD_MS);
                }
                disconnect(tch);
                return;
            }
            if (action_len == strlen("disconnect_touch") && strncmp(action, "disconnect_touch", strlen("disconnect_touch")) == 0) {
                ESP_LOGI(TAG, "disconnect touch");
                disconnect(tch);
                return;
            }
            if (action_len == strlen("reconnect_touch") && strncmp(action, "reconnect_touch", strlen("reconnect_touch")) == 0) {
                ESP_LOGI(TAG, "reconnect touch");
                wake_up(tch);
                return;
            }
            if (action_len == strlen("set_lock_position") && strncmp(action, "set_lock_position", strlen("set_lock_position")) == 0) {
                ssm->mech.lock_unlock.lock = (int) get_json_value(event->data, quote_index, 6, 7);
                ESP_LOGI(TAG, "set lock position to %d", ssm->mech.lock_unlock.lock);
                ssm_mech(ssm, ssm->mech.lock_unlock.lock, ssm->mech.lock_unlock.unlock);
                return;
            }
            if (action_len == strlen("set_unlock_position") && strncmp(action, "set_unlock_position", strlen("set_unlock_position")) == 0) {
                ssm->mech.lock_unlock.unlock = (int) get_json_value(event->data, quote_index, 6, 7);
                ESP_LOGI(TAG, "set unlock position to %d", ssm->mech.lock_unlock.unlock);
                ssm_mech(ssm, ssm->mech.lock_unlock.lock, ssm->mech.lock_unlock.unlock);
                return;
            }
            if (action_len == strlen("battery_update") && strncmp(action, "battery_update", strlen("battery_update")) == 0) {
                ESP_LOGI(TAG, "touch battery update");
                if (wake_up(tch)) {
                    ESP_LOGI(TAG, "touch battery update done");
                } else {
                    ESP_LOGW(TAG, "touch battery update fail");
                }
                disconnect(tch);
                return;
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt_discovery(void) {
    int cnt = 0;
    int msg_id = 0;

    char topic[80];
    char payload[600];

    // char secret[33];
    char * mac_addr;
    for (int n = 0; n < cnt_ssms; n++) {
        sesame * ssm = &(p_ssms_env + n)->ssm;
        mac_addr = addr_str(ssm->addr);
        if (ssm->product_type == SESAME_5 || ssm->product_type == SESAME_5_PRO) {
            // config lock
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_lock\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{\\\"action\\\": \\\"{{ value }}\\\", \\\"code\\\": \\\"{{ code }}\\\"}\",\n");
            cnt += sprintf(payload + cnt, "\"pl_lock\": \"lock\",\n");
            cnt += sprintf(payload + cnt, "\"pl_unlk\": \"unlock\",\n");
            cnt += sprintf(payload + cnt, "\"state_locked\": \"LOCK\",\n");
            cnt += sprintf(payload + cnt, "\"state_unlocked\": \"UNLOCK\",\n");
            cnt += sprintf(payload + cnt, "\"state_jammed\": \"JAMMED\",\n");
            cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.state }}\",\n");
            cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            ESP_LOGI(TAG, "max payload = %d", cnt);
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/lock/%s/config", ssm->topic);            // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt lock config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config battery sensor
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Battery\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_battery\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.battery }}\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"%%\",\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/sensor/%s_battery/config", ssm->topic);  // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt battery config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config position sensor
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Position\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_position\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.position }}\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"°\",\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/sensor/%s_position/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt position config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config number of lock position
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Position Lock\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_lock_position\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set/lock_position\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"set_lock_position\\\", \\\"lock\\\": \\\"{{ value }}\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.lock_position }}\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"°\",\n");
            cnt += sprintf(payload + cnt, "\"mode\": \"box\",\n");
            cnt += sprintf(payload + cnt, "\"max\": 540,\n");
            cnt += sprintf(payload + cnt, "\"min\": -180,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": true,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/number/%s_lock_position/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);      // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt lock_position config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config number of unlock position
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Position Unlock\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_unlock_position\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set/unlock_position\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"set_unlock_position\\\", \\\"unlock\\\": \\\"{{ value }}\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.unlock_position }}\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"°\",\n");
            cnt += sprintf(payload + cnt, "\"mode\": \"box\",\n");
            cnt += sprintf(payload + cnt, "\"max\": 540,\n");
            cnt += sprintf(payload + cnt, "\"min\": -180,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": true,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/number/%s_unlock_position/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);        // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt unlock_position config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config button for horizon calibration
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Horizon Calibration\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_horizon_calibration\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"magnet\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/button/%s_horizon_calibration/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);            // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt horizon calibration config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config text for QR code text
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Gened QR Code Text\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_qr_code_text\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"gen_qr_code_text\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state/qr_code_text\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/text/%s_qr_code_text/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);   // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt qr code text config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config button for QR code text request
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Gen QR Code Text\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_gen_qr_code_text\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"gen_qr_code_text\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/button/%s_gen_qr_code_text/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);         // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt gen qr code text config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);
        } else if (ssm->product_type == SESAME_TOUCH || ssm->product_type == SESAME_TOUCH_PRO) {
            // config battery sensor
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Battery\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_battery\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"%%\",\n");
            cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.battery }}\",\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/sensor/%s_battery/config", ssm->topic);  // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt battery config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            //// config add card switch
            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            // memset(payload, 0, sizeof(payload));
            // cnt = 0;
            // cnt += sprintf(payload + cnt, "{\n");
            // cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            // cnt += sprintf(payload + cnt, "\"name\": \"Add Card\",\n");
            // cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_add_card\",\n", ssm->topic);
            // cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            //// cnt += sprintf(payload+cnt, "\"cmd_tpl\": \"{\\\"action\\\": \\\"{{ value }}\\\"}\",\n");
            // cnt += sprintf(payload + cnt, "\"payload_on\": \"add_card\",\n");
            // cnt += sprintf(payload + cnt, "\"payload_off\": \"verify_card\",\n");
            // cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            // cnt += sprintf(payload + cnt, "\"state_on\": \"1\",\n");
            // cnt += sprintf(payload + cnt, "\"state_off\": \"0\",\n");
            // cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.add_card }}\",\n");
            // cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            // cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            // cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            // cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            // cnt += sprintf(payload + cnt, "\"dev\": {\n");
            //// cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            // cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            // cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            // cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            // cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            // cnt += sprintf(payload + cnt, "}\n");
            // cnt += sprintf(payload + cnt, "}");
            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/switch/%s_add_card/config", ssm->topic); // config topic
            // msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);					// QOS 2, retain 1
            // ESP_LOGI(TAG, "sent mqtt add card config for %s, msg_id=%d", ssm->topic, msg_id);
            // wait_published(msg_id);

            //// config add finger switch
            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            // memset(payload, 0, sizeof(payload));
            // cnt = 0;
            // cnt += sprintf(payload + cnt, "{\n");
            // cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            // cnt += sprintf(payload + cnt, "\"name\": \"Add Finger\",\n");
            // cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_add_finger\",\n", ssm->topic);
            // cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            //// cnt += sprintf(payload+cnt, "\"cmd_tpl\": \"{\\\"action\\\": \\\"{{ value }}\\\"}\",\n");
            // cnt += sprintf(payload + cnt, "\"payload_on\": \"add_finger\",\n");
            // cnt += sprintf(payload + cnt, "\"payload_off\": \"verify_finger\",\n");
            // cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            // cnt += sprintf(payload + cnt, "\"state_on\": \"1\",\n");
            // cnt += sprintf(payload + cnt, "\"state_off\": \"0\",\n");
            // cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.add_finger }}\",\n");
            // cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            // cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            // cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            // cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            // cnt += sprintf(payload + cnt, "\"dev\": {\n");
            //// cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            // cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            // cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            // cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            // cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            // cnt += sprintf(payload + cnt, "}\n");
            // cnt += sprintf(payload + cnt, "}");
            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/switch/%s_add_finger/config", ssm->topic); // config topic
            // msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);					  // QOS 2, retain 1
            // ESP_LOGI(TAG, "sent mqtt add finger config for %s, msg_id=%d", ssm->topic, msg_id);
            // wait_published(msg_id);

            //// config connect switch
            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/%s/set/connect", ssm->topic); // command topic
            // msg_id = esp_mqtt_client_subscribe(client_ssm, topic, 2);					 // QOS 2
            // ESP_LOGI(TAG, "sent subscribe for %s, msg_id=%d", ssm->topic, msg_id);
            // wait_published(msg_id);

            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            // memset(payload, 0, sizeof(payload));
            // cnt = 0;
            // cnt += sprintf(payload + cnt, "{\n");
            // cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            // cnt += sprintf(payload + cnt, "\"name\": \"Connect\",\n");
            // cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_connect\",\n", ssm->topic);
            // cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set/connect\",\n");
            //// cnt += sprintf(payload+cnt, "\"cmd_tpl\": \"{\\\"action\\\": \\\"{{ value }}\\\"}\",\n");
            // cnt += sprintf(payload + cnt, "\"payload_on\": \"reconnect_touch\",\n");
            // cnt += sprintf(payload + cnt, "\"payload_off\": \"disconnect_touch\",\n");
            // cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state\",\n");
            // cnt += sprintf(payload + cnt, "\"state_on\": \"1\",\n");
            // cnt += sprintf(payload + cnt, "\"state_off\": \"0\",\n");
            // cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.connect }}\",\n");
            // cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            // cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            // cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            // cnt += sprintf(payload + cnt, "\"retain\": true,\n");
            // cnt += sprintf(payload + cnt, "\"dev\": {\n");
            //// cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            // cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            // cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            // cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            // cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            // cnt += sprintf(payload + cnt, "}\n");
            // cnt += sprintf(payload + cnt, "}");
            // memset(topic, 0, sizeof(topic));
            // sprintf(topic, "homeassistant/switch/%s_connect/config", ssm->topic); // config topic
            // msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);				   // QOS 2, retain 1
            // ESP_LOGI(TAG, "sent mqtt connect config for %s, msg_id=%d", ssm->topic, msg_id);
            // wait_published(msg_id);

            // config text for add sesame
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Enter MAC address - Add Sesame\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_add_sesame\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"add_sesame\\\", \\\"mac\\\": \\\"{{ value }}\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state/add_sesame\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/text/%s_add_sesame/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt add sesame config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config text for remove sesame
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Enter MAC address - Remove Sesame\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_remove_sesame\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"remove_sesame\\\", \\\"mac\\\": \\\"{{ value }}\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state/remove_sesame\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/text/%s_remove_sesame/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);    // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt remove sesame config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config text for QR code text
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Gened QR Code Text\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_qr_code_text\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"gen_qr_code_text\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"stat_t\": \"~/state/qr_code_text\",\n");
            // cnt += sprintf(payload + cnt, "\"value_template\": \"{{ value_json.connect }}\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"optimistic\": false,\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/text/%s_qr_code_text/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);   // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt qr code text config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config button for QR code text request
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Gen QR Code Text\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_gen_qr_code_text\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"gen_qr_code_text\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/button/%s_gen_qr_code_text/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);         // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt gen qr code text config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // config button for Battery update
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s", ssm->topic); // base topic
            memset(payload, 0, sizeof(payload));
            cnt = 0;
            cnt += sprintf(payload + cnt, "{\n");
            cnt += sprintf(payload + cnt, "\"~\": \"%s\",\n", topic);
            cnt += sprintf(payload + cnt, "\"name\": \"Battery Update\",\n");
            cnt += sprintf(payload + cnt, "\"uniq_id\": \"%s_battery_update\",\n", ssm->topic);
            cnt += sprintf(payload + cnt, "\"cmd_t\": \"~/set\",\n");
            cnt += sprintf(payload + cnt, "\"cmd_tpl\": \"{ \\\"action\\\": \\\"battery_update\\\" }\",\n");
            cnt += sprintf(payload + cnt, "\"unit_of_measurement\": \"\",\n");
            cnt += sprintf(payload + cnt, "\"qos\": 2,\n");
            cnt += sprintf(payload + cnt, "\"retain\": false,\n");
            cnt += sprintf(payload + cnt, "\"dev\": {\n");
            // cnt += sprintf(payload + cnt, "\"ids\": \"%s\",\n", secret);
            cnt += sprintf(payload + cnt, "\"connections\": [[\"mac\", \"%s\"]],\n", mac_addr);
            cnt += sprintf(payload + cnt, "\"mf\": \"Candy House\",\n");
            cnt += sprintf(payload + cnt, "\"name\": \"%s\",\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "\"mdl\": \"%s\"\n", SSM_PRODUCT_TYPE_STR(ssm->product_type));
            cnt += sprintf(payload + cnt, "}\n");
            cnt += sprintf(payload + cnt, "}");
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/button/%s_battery_update/config", ssm->topic); // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, payload, 0, 2, 1);       // QOS 2, retain 1
            ESP_LOGI(TAG, "sent mqtt battery update config for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);
        }
    }

    mqtt_discovery_done = 1;
    ESP_LOGI(TAG, "MQTT discovery done.");
}

void mqtt_subscribe(void) {
    int msg_id = 0;
    char topic[80];
    char empty_payload[2] = "";
    for (int n = 0; n < cnt_ssms; n++) { // subscribe
        sesame * ssm = &(p_ssms_env + n)->ssm;
        memset(topic, 0, sizeof(topic));
        sprintf(topic, "homeassistant/%s/set", ssm->topic);                     // command topic
        if (ssm->is_new) {                                                      // if just registered, clear the retained message before subscribe
            msg_id = esp_mqtt_client_publish(client_ssm, topic, NULL, 0, 0, 1); // QOS 0, retain 1
            ESP_LOGI(TAG, "clear retained message for %s, msg_id=%d", topic, msg_id);
        }
        msg_id = esp_mqtt_client_subscribe(client_ssm, topic, 2); // QOS 2
        ESP_LOGI(TAG, "sent subscribe for %s, msg_id=%d", ssm->topic, msg_id);
        // wait_published(msg_id);

        memset(topic, 0, sizeof(topic));
        sprintf(topic, "homeassistant/%s/state", ssm->topic);                   // command topic
        if (ssm->is_new) {                                                      // if just registered, clear the retained message before subscribe
            msg_id = esp_mqtt_client_publish(client_ssm, topic, NULL, 0, 0, 1); // QOS 0, retain 1
            ESP_LOGI(TAG, "clear retained message for %s, msg_id=%d", topic, msg_id);
        }

        // set empty default value for qr code test for both Sesame Lock and Touch
        memset(topic, 0, sizeof(topic));
        sprintf(topic, "homeassistant/%s/state/qr_code_text", ssm->topic);           // config topic
        msg_id = esp_mqtt_client_publish(client_ssm, topic, empty_payload, 0, 2, 0); // QOS 2, retain 1
        ESP_LOGI(TAG, "sent \"\" for qr_code_text for %s, msg_id=%d", ssm->topic, msg_id);
        wait_published(msg_id);

        if (ssm->product_type == SESAME_5 || ssm->product_type == SESAME_5_PRO) {
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s/set/lock_position", ssm->topic);       // command topic
            if (ssm->is_new) {                                                      // if just registered, clear the retained message before subscribe
                msg_id = esp_mqtt_client_publish(client_ssm, topic, NULL, 0, 0, 1); // QOS 0, retain 1
                ESP_LOGI(TAG, "clear retained message for %s, msg_id=%d", ssm->topic, msg_id);
            }
            msg_id = esp_mqtt_client_subscribe(client_ssm, topic, 2); // QOS 2
            ESP_LOGI(TAG, "sent subscribe for %s, msg_id=%d", ssm->topic, msg_id);
            // wait_published(msg_id);

            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s/set/unlock_position", ssm->topic);     // command topic
            if (ssm->is_new) {                                                      // if just registered, clear the retained message before subscribe
                msg_id = esp_mqtt_client_publish(client_ssm, topic, NULL, 0, 0, 1); // QOS 0, retain 1
                ESP_LOGI(TAG, "clear retained message for %s, msg_id=%d", ssm->topic, msg_id);
            }
            msg_id = esp_mqtt_client_subscribe(client_ssm, topic, 2); // QOS 2
            ESP_LOGI(TAG, "sent subscribe for %s, msg_id=%d", ssm->topic, msg_id);
            // wait_published(msg_id);
        } else {
            // set empty default value for add_sesame for Touch
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s/state/add_sesame", ssm->topic);             // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, empty_payload, 0, 2, 0); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent \"\" for add_sesame for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);

            // set empty default value for remove_sesame for Touch
            memset(topic, 0, sizeof(topic));
            sprintf(topic, "homeassistant/%s/state/remove_sesame", ssm->topic);          // config topic
            msg_id = esp_mqtt_client_publish(client_ssm, topic, empty_payload, 0, 2, 0); // QOS 2, retain 1
            ESP_LOGI(TAG, "sent \"\" for remove_sesame for %s, msg_id=%d", ssm->topic, msg_id);
            wait_published(msg_id);
        }
    }

    for (int n = 0; n < cnt_ssms; n++) { // update device status to HA and disconnect sesame touch or touch pro for power saving
        (p_ssms_env + n)->ssm_cb__(&(p_ssms_env + n)->ssm);
        if ((p_ssms_env + n)->ssm.product_type == SESAME_TOUCH || (p_ssms_env + n)->ssm.product_type == SESAME_TOUCH_PRO) { // disconnect sesame touch or touch pro for power saving
            disconnect(&(p_ssms_env + n)->ssm);
        }
        ESP_LOGW(TAG, "id = %d, conn_id = %d, been found %d times", (p_ssms_env + n)->ssm.id, (p_ssms_env + n)->ssm.conn_id, (p_ssms_env + n)->ssm.cnt_discovery);
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        //.broker.address.uri = CONFIG_BROKER_URL,
        .broker.address.uri = config_broker_url,
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    // esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    client_ssm = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client_ssm, ESP_EVENT_ANY_ID, mqtt_event_handler, client_ssm);
    esp_mqtt_client_start(client_ssm);
}

void mqtt_start(void) {
    mqtt_app_start();
    for (int n = 0; n < 600; n++) { // timeout after 60 seconds
        if (mqtt_init_done) {
            ESP_LOGI(TAG, "MQTT init done. Takes at %f seconds", n * 0.1);
            break;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
