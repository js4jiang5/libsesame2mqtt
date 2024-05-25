#ifndef __MQTT_SECTION_H__
#define __MQTT_SECTION_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "mqtt_client.h"

extern int mqtt_init_done;
extern int msg_id_subscribed;
extern char config_broker_url[60];
extern esp_mqtt_client_handle_t client_ssm;

int wait_published(int msg_id);

int wake_up(sesame * ssm);

void mqtt_start(void);

void mqtt_discovery(void);

void mqtt_subscribe(void);

#ifdef __cplusplus
}
#endif
#endif // __MQTT_SECTION_H__