#include <errno.h>
#include <zephyr.h>
#include <drivers/gpio.h>
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"

#include <stdio.h>
#include <string.h>
#include <net/coap.h>

#include <power.h>
#include <net/net_if.h>
#include <net/openthread.h>
#include <openthread/thread.h>

#include "zcoap.h"
#include "degu_utils.h"
#include "degu_ota.h"
#include "degu_pm.h"

#define GPIO_CFG_SENSE_LOW (GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos)

STATIC mp_obj_t degu_check_update(void) {
	return mp_obj_new_int(check_update());
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(degu_check_update_obj, degu_check_update);

STATIC mp_obj_t degu_update_shadow(mp_obj_t shadow) {
	int ret = degu_coap_request("thing", COAP_METHOD_POST, (u8_t *)mp_obj_str_get_str(shadow), NULL);

	return mp_obj_new_int(ret);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(degu_update_shadow_obj, degu_update_shadow);

STATIC mp_obj_t degu_get_shadow(void) {
	vstr_t vstr;
	int ret;
	u8_t *payload = (u8_t *)m_malloc(MAX_COAP_MSG_LEN);

	if (!payload) {
		printf("can't malloc\n");
		return mp_const_none;
	}
	memset(payload, 0, MAX_COAP_MSG_LEN);

	ret = degu_coap_request("thing", COAP_METHOD_GET, payload, NULL);

	if (payload != NULL && ret >= COAP_RESPONSE_CODE_OK) {
		vstr_init_len(&vstr, strlen(payload));
		strcpy(vstr.buf, payload);
		m_free(payload);
		return mp_obj_new_str_from_vstr(&mp_type_str, &vstr);
	}
	else {
		m_free(payload);
		return mp_const_none;
	}
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(degu_get_shadow_obj, degu_get_shadow);

STATIC mp_obj_t mod_suspend(size_t n_args, mp_obj_t *args)
{
	s32_t time_to_wake = mp_obj_get_int(args[0]);
	bool external_awake = n_args >= 2 && mp_obj_is_true(args[1]);

	struct net_if *iface;
	struct openthread_context *ot_context;
	otLinkModeConfig config;
	uint8_t channel;

	iface = net_if_get_default();
	ot_context = net_if_l2_data(iface);
	channel = otLinkGetChannel(ot_context->instance);
	config = otThreadGetLinkMode(ot_context->instance);

#ifdef CONFIG_SYS_POWER_MANAGEMENT
	if (external_awake) {
		sys_pm_ctrl_enable_state(SYS_POWER_STATE_SLEEP_1);
		sys_set_power_state(SYS_POWER_STATE_SLEEP_1);
	} else {
		sys_pm_ctrl_enable_state(SYS_POWER_STATE_SLEEP_3);
		sys_set_power_state(SYS_POWER_STATE_SLEEP_3);
	}
#endif

	openthread_suspend(ot_context->instance);
	k_sleep(K_SECONDS(time_to_wake));
	openthread_resume(ot_context->instance, channel, config);

#ifdef CONFIG_SYS_POWER_MANAGEMENT
	if (external_awake) {
		sys_pm_ctrl_disable_state(SYS_POWER_STATE_SLEEP_1);
	} else {
		sys_pm_ctrl_disable_state(SYS_POWER_STATE_SLEEP_3);
	}
#endif

	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_suspend_obj, 1, 2, mod_suspend);

STATIC bool listen_to_gpio(mp_obj_t gpio) {
	mp_obj_t *items;
	mp_obj_get_array_fixed_n(gpio, 2, &items);

	const char *drv_name = mp_obj_str_get_str(items[0]);
	int pin = mp_obj_get_int(items[1]);
	struct device *port = device_get_binding(drv_name);
    if (!port) {
        return false;
    }

	gpio_pin_configure(port, pin, GPIO_DIR_IN 
		| GPIO_PUD_PULL_UP
		| GPIO_INT | GPIO_INT_LEVEL
		| GPIO_CFG_SENSE_LOW);
	gpio_pin_enable_callback(port, pin);

	return true;
}

STATIC mp_obj_t mod_powerdown(size_t n_args, mp_obj_t *args)
{
	bool external_awake = n_args >= 1 && mp_obj_is_true(args[0]);

	if (n_args >= 2) {
		if (!external_awake) {
			mp_raise_ValueError("unable to listen to external gpio when external device power is down");
		}

		if (mp_obj_is_type(args[1], &mp_type_tuple)) {
			if (!listen_to_gpio(args[1])) {
				mp_raise_ValueError("the specified port is invalid");
			}
		} else if (mp_obj_is_type(args[1], &mp_type_list)) {
			size_t listener_arr_size;
			mp_obj_t* listener_arr;
			mp_obj_get_array(args[1], &listener_arr_size, &listener_arr);

			for (size_t i = 0; i < listener_arr_size; i++) {
				if (!mp_obj_is_type(listener_arr[i], &mp_type_tuple)) {
					mp_raise_ValueError("one or more of the listeners are not tuples");
				}
				if (!listen_to_gpio(listener_arr[i])) {
					mp_raise_ValueError("the specified port is invalid");
				}
			}
		} else {
			mp_raise_ValueError("the listener must be a tuple, (\"GPIO_x\", pin), or their list");
		}
	}

	degu_ext_device_power(external_awake);
#ifdef CONFIG_DEVICE_POWER_MANAGEMENT
	sys_pm_suspend_devices();
#endif

#ifdef CONFIG_SYS_POWER_MANAGEMENT
	sys_pm_ctrl_enable_state(SYS_POWER_STATE_DEEP_SLEEP_1);
	sys_set_power_state(SYS_POWER_STATE_DEEP_SLEEP_1);
	sys_pm_ctrl_disable_state(SYS_POWER_STATE_DEEP_SLEEP_1);
#endif

#ifdef CONFIG_DEVICE_POWER_MANAGEMENT
	sys_pm_resume_devices();
#endif
	return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_powerdown_obj, 0, 2, mod_powerdown);

STATIC const mp_rom_map_elem_t degu_globals_table[] = {
	{MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_degu) },
	{ MP_ROM_QSTR(MP_QSTR_check_update), MP_ROM_PTR(&degu_check_update_obj) },
	{ MP_ROM_QSTR(MP_QSTR_update_shadow), MP_ROM_PTR(&degu_update_shadow_obj) },
	{ MP_ROM_QSTR(MP_QSTR_get_shadow), MP_ROM_PTR(&degu_get_shadow_obj) },
	{ MP_ROM_QSTR(MP_QSTR_suspend), MP_ROM_PTR(&mod_suspend_obj) },
	{ MP_ROM_QSTR(MP_QSTR_powerdown), MP_ROM_PTR(&mod_powerdown_obj) },
};

STATIC MP_DEFINE_CONST_DICT (mp_module_degu_globals, degu_globals_table);

const mp_obj_module_t mp_module_degu = {
	.base = { &mp_type_module },
	.globals = (mp_obj_dict_t*)&mp_module_degu_globals,
};
