/**
 ****************************************************************************************
 *
 * @file user_social_distancing.c
 *
 * @brief Social distancing software example source code.
 *
 * Copyright (c) 2015-2020 Dialog Semiconductor. All rights reserved.
 *
 * This software ("Software") is owned by Dialog Semiconductor.
 *
 * By using this Software you agree that Dialog Semiconductor retains all
 * intellectual property and proprietary rights in and to this Software and any
 * use, reproduction, disclosure or distribution of the Software without express
 * written permission or a license agreement from Dialog Semiconductor is
 * strictly prohibited. This Software is solely for use on or in conjunction
 * with Dialog Semiconductor products.
 *
 * EXCEPT AS OTHERWISE PROVIDED IN A LICENSE AGREEMENT BETWEEN THE PARTIES, THE
 * SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. EXCEPT AS OTHERWISE
 * PROVIDED IN A LICENSE AGREEMENT BETWEEN THE PARTIES, IN NO EVENT SHALL
 * DIALOG SEMICONDUCTOR BE LIABLE FOR ANY DIRECT, SPECIAL, INDIRECT, INCIDENTAL,
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THE SOFTWARE.
 *
 ****************************************************************************************
 */

/**
 ****************************************************************************************
 * @addtogroup APP
 * @{
 ****************************************************************************************
 */

/*
 * INCLUDE FILES
 ****************************************************************************************
 */

#include "rwip_config.h"             // SW configuration
#include "gap.h"
#include "app_mid.h"
#include "app_user_config.h"
#include "app_easy_timer.h"
#include "user_social_distancing.h"
#include "user_custs1_def.h"
#include "co_bt.h"
#include "arch_console.h"

/*
 * DEFINES
 ****************************************************************************************
 */

#define USER_CON_INTV        30  //ms


/*
 * GLOBAL VARIABLE DEFINITIONS
 ****************************************************************************************
 */

uint8_t app_connection_idx                      __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY
timer_hnd app_param_update_request_timer_used   __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY

timer_hnd user_switch_adv_scan_timer            __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY

/*
 * LOCAL VARIABLE DEFINITIONS
 ****************************************************************************************
 */

static bool is_user_connected                   __SECTION_ZERO("retention_mem_area0"); //@RETENTION MEMORY

static const int8_t user_prox_zones_rssi[USER_PROX_ZONE_COUNT] = {-50, -40, -30, -20};

static struct user_adv_rssi_node* user_adv_rep_rssi_head = NULL;

struct scan_configuration {
    /// Operation code.
    uint8_t     code;
    /// Own BD address source of the device
    uint8_t     addr_src;
    /// Scan interval
    uint16_t    interval;
    /// Scan window size
    uint16_t    window;
    /// Scanning mode
    uint8_t     mode;
    /// Scan filter policy
    uint8_t     filt_policy;
    /// Scan duplicate filtering policy
    uint8_t     filter_duplic;
};

static const struct scan_configuration user_scan_conf ={
    /// Operation code.
    .code = GAPM_SCAN_PASSIVE,
    /// Own BD address source of the device
    .addr_src = GAPM_STATIC_ADDR,
    /// Scan interval
    .interval = (USER_ADVERTISING_INT_MAX * 3) / 2,
    /// Scan window size
    .window = USER_ADVERTISING_INT_MIN/3,
    /// Scanning mode
    .mode = GAP_GEN_DISCOVERY,
    /// Scan filter policy
    .filt_policy = SCAN_ALLOW_ADV_ALL,
    /// Scan duplicate filtering policy
    .filter_duplic = SCAN_FILT_DUPLIC_EN
};

static const uint8_t user_custom_srv_uuid[] = DEF_SVC1_UUID_128;

/*
 * FUNCTION DEFINITIONS
 ****************************************************************************************
*/
static struct user_adv_rssi_node* user_adv_rssi_create_node()
{
    if (ke_check_malloc(sizeof(struct user_adv_rssi_node), KE_MEM_ENV))
    {
        struct user_adv_rssi_node* temp;
        
        temp = (struct user_adv_rssi_node *)ke_malloc(sizeof(struct user_adv_rssi_node), KE_MEM_ENV);
        memset(&temp->adv_addr, 0, sizeof(temp->adv_addr));
        memset(&temp->adv_addr_type, 0, sizeof(temp->adv_addr_type));
        memset(&temp->count, 0, sizeof(temp->count));
        memset(&temp->mean_rssi, 0, sizeof(temp->mean_rssi));
        temp->next = NULL;
        
        return temp;
    }
    
    return NULL;
}

static struct user_adv_rssi_node* user_adv_rssi_add_node_rssi(struct gapm_adv_report_ind const * adv_report)
{
    struct user_adv_rssi_node* p;
    struct user_adv_rssi_node* temp;
    
    temp = user_adv_rssi_create_node();
    memcpy(&temp->adv_addr_type, &adv_report->report.adv_addr_type, sizeof(temp->adv_addr_type));
    memcpy(&temp->adv_addr, &adv_report->report.adv_addr, sizeof(temp->adv_addr));
    memcpy(&temp->mean_rssi, &adv_report->report.rssi, sizeof(temp->mean_rssi));
    (temp->count)++;
    temp->next = NULL;
    
    if (user_adv_rep_rssi_head == NULL) // HEAD is NULL
    {
        user_adv_rep_rssi_head = temp;
    }
    else
    {
        p = user_adv_rep_rssi_head;
        while(memcmp(&p->adv_addr, &adv_report->report.adv_addr, sizeof(p->adv_addr))
              || p->adv_addr_type != adv_report->report.adv_addr_type) 
        {
            if (p->next != NULL)
                p = p->next;
            else
                break;
        }
        
        if (!memcmp(&p->adv_addr, &adv_report->report.adv_addr, sizeof(p->adv_addr))
            && p->adv_addr_type == adv_report->report.adv_addr_type)
        {
            p->mean_rssi = (p->count * p->mean_rssi + adv_report->report.rssi) / (p->count + 1);
            (p->count)++;
        }
        else
        {
            p->next = temp;
        }
    }
}

static void user_scan_start(void)
{
    struct gapm_start_scan_cmd* cmd = KE_MSG_ALLOC(GAPM_START_SCAN_CMD,
                                                   TASK_GAPM,
                                                   TASK_APP,
                                                   gapm_start_scan_cmd);

    cmd->op.code = user_scan_conf.code;
    cmd->op.addr_src = user_scan_conf.addr_src;
    cmd->interval = user_scan_conf.interval;
    cmd->window = user_scan_conf.window;
    cmd->mode = user_scan_conf.mode;
    cmd->filt_policy = user_scan_conf.filt_policy;
    cmd->filter_duplic =user_scan_conf.filter_duplic;

    // Send the message
    ke_msg_send(cmd);

    // We are now connectable
    ke_state_set(TASK_APP, APP_CONNECTABLE);
}

static void param_update_request_timer_cb()
{
    app_easy_gap_param_update_start(app_connection_idx);
    app_param_update_request_timer_used = EASY_TIMER_INVALID_TIMER;
}

static void user_switch_adv_scan_timer_cb()
{
    user_switch_adv_scan_timer = EASY_TIMER_INVALID_TIMER;
    
    app_easy_gap_advertise_stop(); 
}

void user_app_init(void)
{
    app_param_update_request_timer_used = EASY_TIMER_INVALID_TIMER;
    user_switch_adv_scan_timer = EASY_TIMER_INVALID_TIMER;
    
    default_app_on_init();
}

void user_app_on_scanning_completed(const uint8_t param)
{
    if (!is_user_connected)
        user_app_adv_start();
    else
        app_easy_gap_start_connection_to();

}

void user_app_adv_start(void)
{
    int8_t rand_val = co_rand_byte();

    user_switch_adv_scan_timer = app_easy_timer(USER_SWITCH_ADV_SCAN_TO + rand_val, user_switch_adv_scan_timer_cb);

    app_easy_gap_undirected_advertise_start();
}

void user_app_connection(uint8_t connection_idx, struct gapc_connection_req_ind const *param)
{
    if (app_env[connection_idx].conidx != GAP_INVALID_CONIDX)
    {
        app_connection_idx = connection_idx;

        // Check if the parameters of the established connection are the preferred ones.
        // If not then schedule a connection parameter update request.
        if ((param->con_interval < user_connection_param_conf.intv_min) ||
            (param->con_interval > user_connection_param_conf.intv_max) ||
            (param->con_latency != user_connection_param_conf.latency) ||
            (param->sup_to != user_connection_param_conf.time_out))
        {
            // Connection params are not these that we expect
            app_param_update_request_timer_used = app_easy_timer(APP_PARAM_UPDATE_REQUEST_TO, param_update_request_timer_cb);
        }
        
        if(user_switch_adv_scan_timer != EASY_TIMER_INVALID_TIMER)
        {    
            app_easy_timer_cancel(user_switch_adv_scan_timer);
        }
    }
    else
    {
        // No connection has been established, restart advertising
        user_app_adv_start();
    }

    default_app_on_connection(connection_idx, param);
}

void user_app_adv_undirect_complete(uint8_t status)
{
    // If advertising was canceled then update advertising data and start advertising again
    if (status == GAP_ERR_CANCELED)
    {
        user_scan_start();
    }
}

void user_app_disconnect(struct gapc_disconnect_ind const *param)
{
    // Cancel the parameter update request timer
    if (app_param_update_request_timer_used != EASY_TIMER_INVALID_TIMER)
    {
        app_easy_timer_cancel(app_param_update_request_timer_used);
        app_param_update_request_timer_used = EASY_TIMER_INVALID_TIMER;
    }

    // Restart Advertising
    user_app_adv_start();
}

void user_catch_rest_hndl(ke_msg_id_t const msgid,
                          void const *param,
                          ke_task_id_t const dest_id,
                          ke_task_id_t const src_id)
{
    switch(msgid)
    {
        case GAPC_PARAM_UPDATED_IND:
        {
            // Cast the "param" pointer to the appropriate message structure
            struct gapc_param_updated_ind const *msg_param = (struct gapc_param_updated_ind const *)(param);

            // Check if updated Conn Params filled to preferred ones
            if ((msg_param->con_interval >= user_connection_param_conf.intv_min) &&
                (msg_param->con_interval <= user_connection_param_conf.intv_max) &&
                (msg_param->con_latency == user_connection_param_conf.latency) &&
                (msg_param->sup_to == user_connection_param_conf.time_out))
            {
            }
        } break;

        default:
            break;
    }
}

/**
 ****************************************************************************************
 * @brief Handles advertise reports
 * @param[in]   param Parameters of advertise report message
 * @return void
 ****************************************************************************************
 */
void user_app_on_adv_report_ind(struct gapm_adv_report_ind const * param)
{
    uint8_t report_data[ADV_DATA_LEN + 1];
    
    memcpy(report_data, param->report.data, param->report.data_len);
    report_data[param->report.data_len] = '\0';
    
    arch_printf("\r\n%02x %02x %02x %02x %02x %02x\t",
    param->report.adv_addr.addr[5],
    param->report.adv_addr.addr[4],
    param->report.adv_addr.addr[3],
    param->report.adv_addr.addr[2],
    param->report.adv_addr.addr[1],
    param->report.adv_addr.addr[0]);
    arch_printf("%d\t", (int8_t)param->report.rssi);
    
    if(!memcmp(report_data + 9, user_custom_srv_uuid, ATT_UUID_128_LEN))
    {
        arch_printf("TRUE");
    
        if((int8_t)param->report.rssi > -50)
        {
            is_user_connected = true;
            
            app_easy_gap_start_connection_to_set(param->report.adv_addr_type, (uint8_t *)&param->report.adv_addr.addr, MS_TO_DOUBLESLOTS(USER_CON_INTV));
        }
    }
    else
        arch_printf("FALSE");

    // Populate advertiser report list
    
}
/// @} APP
