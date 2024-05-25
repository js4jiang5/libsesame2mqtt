#ifndef __WIFI_SECTION_H__
#define __WIFI_SECTION_H__

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_sta(void);
extern int wifi_connected_to_ap;
extern char config_wifi_ssid[32];
extern char config_wifi_password[64];

#ifdef __cplusplus
}
#endif

#endif // __WIFI_SECTION_H__