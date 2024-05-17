#ifndef __SSM_CMD_H__
#define __SSM_CMD_H__

#include "ssm.h"

#ifdef __cplusplus
extern "C" {
#endif

void send_reg_cmd_to_ssm(sesame * ssm);

void handle_reg_data_from_ssm(sesame * ssm);

void send_login_cmd_to_ssm(sesame * ssm);

void send_read_history_cmd_to_ssm(sesame * ssm);

void ssm_lock(sesame * ssm, uint8_t * tag, uint8_t tag_length);

void ssm_unlock(sesame * ssm, uint8_t * tag, uint8_t tag_length);

void ssm_magnet(sesame * ssm);

void ssm_mech(sesame * ssm, int lock_position, int unlock_position);

void tch_add_sesame(sesame * tch, sesame * ssm);

void tch_remove_sesame(sesame * tch, sesame * ssm);

void tch_finger_add(sesame * tch);

void tch_finger_verify(sesame * tch);

void tch_finger_mode_get(sesame * tch);

void tch_finger_get(sesame * tch);

void tch_card_add(sesame * tch);

void tch_card_verify(sesame * tch);

void tch_card_mode_get(sesame * tch);

void tch_card_get(sesame * tch);

#ifdef __cplusplus
}
#endif

#endif // __SSM_CMD_H__
