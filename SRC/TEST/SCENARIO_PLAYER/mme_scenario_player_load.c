/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <libxml/xmlwriter.h>
#include <libxml/xpath.h>
#include "bstrlib.h"

#include "xml_load.h"
#include "log.h"
#include "msc.h"
#include "assertions.h"
#include "conversions.h"
#include "mme_scenario_player.h"
#include "3gpp_23.003.h"
#include "3gpp_24.008.h"
#include "3gpp_33.401.h"
#include "3gpp_24.007.h"
#include "3gpp_36.401.h"
#include "3gpp_36.331.h"
#include "3gpp_24.301.h"
#include "security_types.h"
#include "securityDef.h"
#include "common_types.h"
#include "mme_api.h"
#include "emm_data.h"
#include "emm_msg.h"
#include "esm_msg.h"
#include "intertask_interface.h"
#include "timer.h"
#include "dynamic_memory_check.h"
#include "common_defs.h"
#include "xml_msg_tags.h"
#include "xml_msg_load_itti.h"
#include "itti_free_defined_msg.h"
#include "itti_getters.h"

extern scenario_player_t g_msp_scenarios;

//------------------------------------------------------------------------------
getter_f msp_get_getter_function_from_var_name(bstring var_name)
{
  if (biseqcstrcaseless(var_name, "ITTI_NAS_DOWNLINK_DATA_REQ.MME_UE_S1AP_ID"))
    return nas_dl_data_req_get_mme_ue_s1ap_id;
  if (biseqcstrcaseless(var_name, "ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF.KENB"))
    return mme_app_connection_establishment_cnf_get_kenb;
  if (biseqcstrcaseless(var_name, "ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF.ERAB.S1U_SGW_TRANSPORT_LAYER_ADDRESS"))
    return mme_app_connection_establishment_cnf_get_erab0_transport_layer_address;
  if (biseqcstrcaseless(var_name, "ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF.ERAB.S1U_TEID"))
    return mme_app_connection_establishment_cnf_get_erab0_teid;
  if (biseqcstrcaseless(var_name, "ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF.NAS.MAC"))
    return mme_app_connection_establishment_cnf_get_nas_mac;
  if (biseqcstrcaseless(var_name, "ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF.NAS.DOWNLINK_SEQUENCE_NUMBER"))
    return mme_app_connection_establishment_cnf_get_nas_downlink_sequence_number;
  if (biseqcstrcaseless(var_name, "ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF.NAS_EMM.GUTI.M_TMSI"))
    return mme_app_connection_establishment_cnf_get_nas_emm_guti_mtmsi;

  return NULL;
}

//------------------------------------------------------------------------------
unsigned long msp_get_seq_uid(void)
{
  // TODO not really thread-safe, but should not interfere with calling threads
  static unsigned long uid = 0;
  __sync_fetch_and_add (&uid, 1);
  return uid;
}

//------------------------------------------------------------------------------
xmlChar * msp_get_attr_file (xmlDocPtr doc, xmlNodePtr cur)
{
  xmlChar *file_path = NULL;

  cur = cur->xmlChildrenNode;
  while (cur != NULL) {
    if ((!xmlStrcmp(cur->name, (const xmlChar *)FILE_ATTR_XML_STR))) {
      file_path = xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
      return file_path;
      // reminder xmlFree(file_path);
    }
    cur = cur->next;
  }
  OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Could not get attr %s from node %s\n", FILE_ATTR_XML_STR, cur->name);
  return NULL;
}

//------------------------------------------------------------------------------
void msp_msg_add_var_to_be_loaded(scenario_t * const scenario, scenario_player_msg_t * const msg, scenario_player_item_t * const var_item)
{
  AssertFatal (SCENARIO_PLAYER_ITEM_VAR      == var_item->item_type, "Wrong item type %d (should be %d)", var_item->item_type, SCENARIO_PLAYER_ITEM_VAR);

  struct load_list_item_s *list_item = calloc(1, sizeof(*list_item));
  AssertFatal(list_item, "Memory alloc failed");
  list_item->value_getter_func = msp_get_getter_function_from_var_name(var_item->u.var.name);
  AssertFatal(list_item->value_getter_func, "TODO: Write getter function");
  list_item->item = var_item;
  if (msg->vars_to_load) {
    list_item->next = msg->vars_to_load;
  }
  msg->vars_to_load = list_item;
  OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Add var %s to be loaded\n", var_item->u.var.name->data);
}
//------------------------------------------------------------------------------
void msp_msg_add_var_listener(scenario_t * const scenario, scenario_player_item_t * const msg_item, scenario_player_item_t * const var_item)
{
  AssertFatal (SCENARIO_PLAYER_ITEM_ITTI_MSG == msg_item->item_type, "Wrong item type %d (should be %d)", msg_item->item_type, SCENARIO_PLAYER_ITEM_ITTI_MSG);
  AssertFatal (SCENARIO_PLAYER_ITEM_VAR      == var_item->item_type, "Wrong item type %d (should be %d)", var_item->item_type, SCENARIO_PLAYER_ITEM_VAR);

  struct list_item_s *list_item = calloc(1, sizeof(*list_item));
  AssertFatal(list_item, "Memory alloc failed");
  list_item->item = msg_item;
  if (var_item->u.var.value_changed_subscribers) {
    list_item->next = var_item->u.var.value_changed_subscribers;
  }
  var_item->u.var.value_changed_subscribers = list_item;
}
//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_message_file (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node, bstring scenario_file_path)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_ITTI_MSG;
  // get a UID
  spi->uid = msp_get_seq_uid();


  // ACTION attribute
  xmlChar *attr = xmlGetProp(node, (const xmlChar *)ACTION_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", ACTION_ATTR_XML_STR, attr);
    if (!strcasecmp((const char*)attr, SEND_VAL_XML_STR)) {
      spi->u.msg.is_tx = true;
    } else if (!strcasecmp((const char*)attr, RECEIVE_VAL_XML_STR)) {
      spi->u.msg.is_tx = false;
    } else {
      AssertFatal(0, "Syntax Error %s=%s  choice in {%s, %s}", ACTION_ATTR_XML_STR, attr, SEND_VAL_XML_STR, RECEIVE_VAL_XML_STR);
    }
    xmlFree(attr);
  } else {
    AssertFatal(0, "Syntax Error %s={0}  choice in {%s, %s} for file %s", ACTION_ATTR_XML_STR, SEND_VAL_XML_STR, RECEIVE_VAL_XML_STR, bdata(scenario_file_path));
  }



  // TIME REF attribute
  attr = xmlGetProp(node, (const xmlChar *)TIME_REF_ATTR_XML_STR);
  if (attr) {
    if (!strcasecmp((const char *)attr, THIS_VAL_XML_STR)) {
      spi->u.msg.time_out_relative_to_msg_uid  = spi->uid;
    } else {
      int       val = 0;
      int ret = sscanf((const char*)attr, "%d", &val);
      AssertFatal(1 == ret, "Could not read %s=%s", TIME_REF_ATTR_XML_STR, attr);
      AssertFatal(0 >= val, "Bad positive value %s=%d", TIME_REF_ATTR_XML_STR, val);
      int counter = -1;
      scenario_player_item_t * cur = scenario->tail_item;

      while ((counter > val) && (cur)){
        cur = cur->previous_item;
      }
      AssertFatal((cur), "Out of range %s=%d", TIME_REF_ATTR_XML_STR, val);
      spi->u.msg.time_out_relative_to_msg_uid = cur->uid;
    }
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s uid=%u\n", TIME_REF_ATTR_XML_STR, attr, spi->u.msg.time_out_relative_to_msg_uid);
    xmlFree(attr);
  }

  // TIME attribute
  attr = xmlGetProp(node, (const xmlChar *)TIME_ATTR_XML_STR);
  if (attr) {
    if (!strcasecmp((const char *)attr, NOW_VAL_XML_STR)) {
      spi->u.msg.time_out.tv_sec  = 0;
      spi->u.msg.time_out.tv_usec = 0;
    } else {
      uint32_t  val1 = 0;
      uint32_t  val2 = 0;
      int ret = sscanf((const char*)attr, "%"SCNu32".%"SCNu32, &val1, &val2);
      if (2 == ret) {
        spi->u.msg.time_out.tv_sec  = val1;
        spi->u.msg.time_out.tv_usec = val2;
      } else if (1 == ret) {
        spi->u.msg.time_out.tv_sec  = val1;
        spi->u.msg.time_out.tv_usec = 0;
      }
    }
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", TIME_ATTR_XML_STR, attr);
    xmlFree(attr);
  } else {
    // by default immediate sent or infinite delay for receiving
    if (spi->u.msg.is_tx) {
      spi->u.msg.time_out.tv_sec  = 0;
      spi->u.msg.time_out.tv_usec = 0;
    } else {
      spi->u.msg.time_out.tv_sec  = 1000;
      spi->u.msg.time_out.tv_usec = 0;
    }
  }

  bstring xpath_expr = bformat("./%s",FILE_ATTR_XML_STR);
  char filename[PATH_MAX];
  bool res = xml_load_leaf_tag(xml_doc, xpath_ctx, xpath_expr, "%s", (void*)filename, NULL);
  if (res) {
    // remove file name from file_path
    bstring bstr = NULL;
    if ('/' == filename[0]) {
      // Absolute path
      bstr = bfromcstr ((const char *)filename);
      int ret = msp_load_message(scenario, bstr, spi);
      bdestroy_wrapper (&bstr);
      if (0 > ret) {
        msp_free_message_content(&spi->u.msg);
        free(spi);
        return NULL;
      }
    } else {
      // relative path
      int pos = bstrrchrp (scenario_file_path, '/', blength(scenario_file_path) - 1);
      if (BSTR_ERR != pos) {
        bstr = bstrcpy(scenario_file_path);
        btrunc(bstr, pos + 1); // keep '/'
        bcatblk(bstr, filename, strlen(filename));
        int ret = msp_load_message(scenario, bstr, spi);
        bdestroy_wrapper (&bstr);
        if (0 > ret) {
          msp_free_message_content(&spi->u.msg);
          free(spi);
          return NULL;
        }
      } else {
        OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Could not find char / in scenarios path %s\n", scenario_file_path->data);
        free(spi);
        return NULL;
      }
    }
  }
  return spi;
}

//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_var (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  bstring var_name = NULL;
  bool    var_already_exist = false;

  xmlChar *attr = xmlGetProp(node, (const xmlChar *)NAME_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found var %s=%s\n", NAME_ATTR_XML_STR, attr);
    var_name  = bfromcstr((const char *)attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find var name");
  }

  scenario_player_item_t * spi = NULL;

  // Check if var already exists
  void                                   *uid_ptr = NULL;
  hashtable_rc_t rc = obj_hashtable_ts_get (scenario->var_items, bdata(var_name), blength(var_name), (void**)&uid_ptr);
  if (HASH_TABLE_OK == rc) {
    bdestroy_wrapper(&var_name);
    int uid = (int)(uintptr_t)uid_ptr;
    hashtable_rc_t hrc = hashtable_ts_get (scenario->scenario_items,
        (hash_key_t)uid, (void **)&spi);
    AssertFatal ((HASH_TABLE_OK == hrc) && (spi), "Could not find var item UID %d", uid);
    AssertFatal (SCENARIO_PLAYER_ITEM_VAR == spi->item_type, "Bad type var item UID %d", spi->item_type);
    var_already_exist = true;
  } else {
    // create it
    spi = calloc(1, sizeof(*spi));
    spi->item_type = SCENARIO_PLAYER_ITEM_VAR;
    // get a UID
    spi->uid = msp_get_seq_uid();
    spi->u.var.name  = var_name;
    var_name = NULL;
    spi->u.var.value.value_u64 = 0;
    spi->u.var.var_ref_uid = 0;
  }



  attr = xmlGetProp(node, (const xmlChar *)VALUE_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found int var %s=%s\n", bdata(spi->u.var.name), attr);
    char * value = (char*)attr;
    if (!strncasecmp("0x", value, 2)) {
      int ret = sscanf((const char*)(attr+2), "%"SCNx64, &spi->u.var.value.value_u64);
      AssertFatal( ret == 1, "Failed to load var %s  hex value %s", bdata(spi->u.var.name), attr);
      spi->u.var.value_type = VAR_VALUE_TYPE_INT64;
    } else if (value[0] == '-') {
      int ret = sscanf((const char*)attr, "%"SCNi64, &spi->u.var.value.value_64);
      AssertFatal( ret == 1, "Failed to load var %s  signed value %s", bdata(spi->u.var.name), attr);
      spi->u.var.value_type = VAR_VALUE_TYPE_INT64;
    } else if (value[0] == '$') {
      void                                   *uid_ref = NULL;
      hashtable_rc_t rc_ref = obj_hashtable_ts_get (scenario->var_items, &value[1], strlen(value) - 1, (void**)&uid_ref);
      AssertFatal (HASH_TABLE_OK == rc_ref, "Could not find ref var %s", &value[1]);
      spi->u.var.var_ref_uid = (int)(uintptr_t)uid_ref;
      // check if uid_ref exist
      rc_ref = hashtable_ts_is_key_exists (scenario->scenario_items, (hash_key_t)spi->u.var.var_ref_uid);
      AssertFatal(HASH_TABLE_OK == rc_ref, "Error in getting uid ref %u", spi->u.var.var_ref_uid);
    } else {
      int ret = sscanf((const char*)attr, "%"SCNu64, &spi->u.var.value.value_u64);
      AssertFatal( ret == 1, "Failed to load var %s  unsigned value %s", bdata(spi->u.var.name), attr);
      spi->u.var.value_type = VAR_VALUE_TYPE_INT64;
    }
    xmlFree(attr);
  } else if ((attr = xmlGetProp(node, (const xmlChar *)ASCII_STREAM_VALUE_ATTR_XML_STR))) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found ascii var %s=%s\n", bdata(spi->u.var.name), attr);
    char * value = (char*)attr;
    spi->u.var.value.value_bstr = bfromcstr((const char *)value);
    spi->u.var.value_type = VAR_VALUE_TYPE_BSTR;
    xmlFree(attr);
  } else if ((attr = xmlGetProp(node, (const xmlChar *)HEX_STREAM_VALUE_ATTR_XML_STR))) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found hex stream var %s=%s\n", bdata(spi->u.var.name), attr);
    char * value = (char*)attr;
    int len = strlen((const char*)value);
    uint8_t hex[len/2];
    int ret = ascii_to_hex ((uint8_t *) hex, (const char *)value);
    AssertFatal(ret, "Could not convert hex stream value");
    spi->u.var.value.value_bstr = blk2bstr(hex,len/2);
    spi->u.var.value_type = VAR_VALUE_TYPE_BSTR;
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find var value type");
  }
  if (!var_already_exist) {
    rc = obj_hashtable_ts_insert (scenario->var_items, bdata(spi->u.var.name), blength(spi->u.var.name), (void*)(uintptr_t)spi->uid);
    AssertFatal(HASH_TABLE_OK == rc, "Error in putting var name in hashtable %d", rc);
  }
  return spi;
}

//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_incr_var (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_VAR_INCR;
  // get a UID
  spi->uid = msp_get_seq_uid();


  void                                   *uid = NULL;
  xmlChar *attr = xmlGetProp(node, (const xmlChar *)NAME_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found incr var %s=%s uid=0x%x\n", NAME_ATTR_XML_STR, attr, spi->uid);
    hashtable_rc_t rc = obj_hashtable_ts_get (scenario->var_items, (const void *)attr, xmlStrlen(attr), (void **)&uid);
    AssertFatal(HASH_TABLE_OK == rc, "Error in getting var %s in hashtable", attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find var name");
  }
  spi->u.uid_incr_var = (int)(uintptr_t)uid;
  return spi;
}
//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_decr_var (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_VAR_DECR;
  // get a UID
  spi->uid = msp_get_seq_uid();

  void                                   *uid = NULL;
  xmlChar *attr = xmlGetProp(node, (const xmlChar *)NAME_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found decr var %s=%s uid=0x%x\n", NAME_ATTR_XML_STR, attr, spi->uid);
    hashtable_rc_t rc = obj_hashtable_ts_get (scenario->var_items, (const void *)attr, xmlStrlen(attr), (void **)&uid);
    AssertFatal(HASH_TABLE_OK == rc, "Error in getting var %s in hashtable", attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find var name");
  }
  spi->u.uid_decr_var = (int)(uintptr_t)uid;
  return spi;
}
//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_sleep (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_SLEEP;
  // get a UID
  spi->uid = msp_get_seq_uid();

  xmlChar *attr = xmlGetProp(node, (const xmlChar *)SECONDS_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found sleep %s seconds\n", attr);
    int ret = sscanf((const char*)attr, "%u", &spi->u.sleep.seconds);
    AssertFatal( ret == 1, "Failed to load sleep value %s", attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find seconds");
  }
  attr = xmlGetProp(node, (const xmlChar *)USECONDS_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found sleep %s useconds\n",attr);
    int ret = sscanf((const char*)attr, "%u", &spi->u.sleep.useconds);
    AssertFatal( ret == 1, "Failed to load sleep value %s", attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find useconds");
  }
  return spi;
}
//------------------------------------------------------------------------------
int msp_load_ue_security_context (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{

  xmlChar *attr = xmlGetProp(node, (const xmlChar *)KSI_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", KSI_ATTR_XML_STR, attr);
    int ret = sscanf((const char*)attr, "%"SCNu8, &((emm_security_context_t*)(scenario->ue_emulated_emm_security_context))->eksi);
    AssertFatal( ret == 1, "Failed to load %s value %s", KSI_ATTR_XML_STR, attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", KSI_ATTR_XML_STR);
  }

  attr = xmlGetProp(node, (const xmlChar *)UL_COUNT_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", UL_COUNT_ATTR_XML_STR, attr);
    uint32_t ul_count = 0;
    int ret = sscanf((const char*)attr, "%u", &ul_count);
    ((emm_security_context_t*)scenario->ue_emulated_emm_security_context)->ul_count.seq_num  = ul_count & 0x000000FF;
    ((emm_security_context_t*)scenario->ue_emulated_emm_security_context)->ul_count.overflow = (ul_count >> 8) & 0x0000FFFF;
    ((emm_security_context_t*)scenario->ue_emulated_emm_security_context)->ul_count.spare    = (ul_count >> 24) & 0x000000FF;

    AssertFatal( ret == 1, "Failed to load %s value %s", UL_COUNT_ATTR_XML_STR, attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", UL_COUNT_ATTR_XML_STR);
  }

  attr = xmlGetProp(node, (const xmlChar *)KNAS_ENC_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", KNAS_ENC_ATTR_XML_STR, attr);
    int len = strlen((const char*)attr);
    uint8_t hex[len/2];
    int ret = ascii_to_hex ((uint8_t *) hex, (const char *)attr);
    if ((ret) && (len/2 == AUTH_KNAS_ENC_SIZE)){
      OAILOG_TRACE (LOG_UTIL, "Set %s=%s\n", KNAS_ENC_ATTR_XML_STR, attr);
      memcpy(((emm_security_context_t*)scenario->ue_emulated_emm_security_context)->knas_enc, hex, len/2);
    } else {
      AssertFatal( ret == 1, "Failed to load %s value %s", KNAS_ENC_ATTR_XML_STR, attr);
    }
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", KNAS_ENC_ATTR_XML_STR);
  }

  attr = xmlGetProp(node, (const xmlChar *)KNAS_INT_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", KNAS_INT_ATTR_XML_STR, attr);
    int len = strlen((const char*)attr);
    uint8_t hex[len/2];
    int ret = ascii_to_hex ((uint8_t *) hex, (const char *)attr);
    if ((ret) && (len/2 == AUTH_KNAS_INT_SIZE)){
      OAILOG_TRACE (LOG_UTIL, "Set %s=%s\n", KNAS_INT_ATTR_XML_STR, attr);
      memcpy(((emm_security_context_t*)scenario->ue_emulated_emm_security_context)->knas_int, hex, len/2);
    } else {
      AssertFatal( ret == 1, "Failed to load %s value %s", KNAS_INT_ATTR_XML_STR, attr);
    }
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", KNAS_INT_ATTR_XML_STR);
  }

  return RETURNok;
}
//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_label (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_LABEL;
  // get a UID
  spi->uid = msp_get_seq_uid();

  xmlChar *attr = xmlGetProp(node, (const xmlChar *)NAME_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found label %s=%s uid=0x%x\n", NAME_ATTR_XML_STR, attr, spi->uid);
    spi->u.label = bfromcstr((const char *)attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", NAME_ATTR_XML_STR);
  }
  hashtable_rc_t rc = obj_hashtable_ts_insert (scenario->label_items, bdata(spi->u.label), blength(spi->u.label), (void*)(uintptr_t)spi->uid);
  AssertFatal(HASH_TABLE_OK == rc, "Error in putting label %s in hashtable %d", bdata(spi->u.label), rc);
  return spi;
}
//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_exit (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_EXIT;
  // get a UID
  spi->uid = msp_get_seq_uid();
  spi->u.exit = SCENARIO_STATUS_NULL;

  xmlChar *attr = xmlGetProp(node, (const xmlChar *)STATUS_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found exit status %s=%s uid=0x%x\n", STATUS_ATTR_XML_STR, attr, spi->uid);
    if (!strcasecmp((const char *)attr, FAILED_VAL_XML_STR)) {
      spi->u.exit = SCENARIO_STATUS_PLAY_FAILED;
    }  else if (!strcasecmp((const char *)attr, SUCCESS_VAL_XML_STR)) {
      spi->u.exit = SCENARIO_STATUS_PLAY_SUCCESS;
    }  else {
      AssertFatal(0, "Exit status not allowed %s", attr);
    }
    xmlFree(attr);
  }
  return spi;
}
//------------------------------------------------------------------------------
scenario_player_item_t* msp_load_jcond (scenario_t * const scenario, xmlDocPtr const xml_doc, xmlXPathContextPtr  xpath_ctx, xmlNodePtr node)
{
  scenario_player_item_t * spi = calloc(1, sizeof(*spi));
  spi->item_type = SCENARIO_PLAYER_ITEM_JUMP_COND;
  xmlChar *attr = xmlGetProp(node, (const xmlChar *)VAR_NAME_ATTR_XML_STR);
  void                                   *uid = NULL;
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s uid=0x%x\n", VAR_NAME_ATTR_XML_STR, attr, spi->uid);
    hashtable_rc_t rc = obj_hashtable_ts_get (scenario->var_items, (const void *)attr, xmlStrlen(attr), (void **)&uid);
    AssertFatal(HASH_TABLE_OK == rc, "Error in getting var %s in hashtable", attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find var_name");
  }
  spi->u.cond.var_uid = (int)(uintptr_t)uid;

  attr = xmlGetProp(node, (const xmlChar *)COND_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s\n", COND_ATTR_XML_STR, attr);
    char * value = (char*)attr;
    if (!strcasecmp("eq", value)) {
      spi->u.cond.test_operator = TEST_EQ;
    } else if (!strcasecmp("ne", value)) {
      spi->u.cond.test_operator = TEST_NE;
    } else  if (!strcasecmp("gt", value)) {
      spi->u.cond.test_operator = TEST_GT;
    } else  if (!strcasecmp("ge", value)) {
      spi->u.cond.test_operator = TEST_GE;
    } else  if (!strcasecmp("lt", value)) {
      spi->u.cond.test_operator = TEST_LT;
    } else  if (!strcasecmp("le", value)) {
      spi->u.cond.test_operator = TEST_LE;
    } else {
      AssertFatal(0, "Could not find understand cond:%s", attr);
    }
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", COND_ATTR_XML_STR);
  }

  attr = xmlGetProp(node, (const xmlChar *)LABEL_ATTR_XML_STR);
  uid = NULL;
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s uid=0x%x\n", LABEL_ATTR_XML_STR, attr, spi->uid);
    hashtable_rc_t rc = obj_hashtable_ts_get (scenario->label_items, (const void *)attr, xmlStrlen(attr), (void **)&uid);
    AssertFatal(HASH_TABLE_OK == rc, "Error in getting label %s in hashtable", attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", LABEL_ATTR_XML_STR);
  }
  spi->u.cond.jump_label_uid = (int)(uintptr_t)uid;

  attr = xmlGetProp(node, (const xmlChar *)VALUE_ATTR_XML_STR);
  uid = NULL;
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s=%s uid=0x%x\n", VALUE_ATTR_XML_STR, attr, spi->uid);
    spi->u.cond.var_test_value = atoi((const char *)attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find %s", VALUE_ATTR_XML_STR);
  }

  return spi;
}

//------------------------------------------------------------------------------
int msp_load_message (scenario_t * const scenario, bstring file_path, scenario_player_item_t * const scenario_player_item)
{
  xmlNodePtr  cur = NULL;
  int         rc = RETURNerror;

  OAILOG_DEBUG (LOG_MME_SCENARIO_PLAYER, "Loading message from XML file %s\n", bdata(file_path));

  if (!scenario_player_item->u.msg.xml_doc) {
    scenario_player_item->u.msg.xml_doc = xmlParseFile(bdata(file_path));
    if (NULL == scenario_player_item->u.msg.xml_doc ) {
      OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Document %s not parsed successfully\n", bdata(file_path));
      return RETURNerror;
    }
  }
  cur = xmlDocGetRootElement(scenario_player_item->u.msg.xml_doc);
  if (cur == NULL) {
    // do not consider this as an error
    OAILOG_WARNING (LOG_MME_SCENARIO_PLAYER, "Empty document %s \n", bdata(file_path));
    xmlFreeDoc(scenario_player_item->u.msg.xml_doc);
    scenario_player_item->u.msg.xml_doc = NULL;
    return RETURNok;
  }

  scenario_player_item->u.msg.xpath_ctx = xmlXPathNewContext(scenario_player_item->u.msg.xml_doc);

  if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_SCTP_NEW_ASSOCIATION_XML_STR)) {
    rc = xml_msg_load_itti_sctp_new_association(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_SCTP_CLOSE_ASSOCIATION_XML_STR)) {
    rc = xml_msg_load_itti_sctp_close_association(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_S1AP_UE_CONTEXT_RELEASE_REQ_XML_STR)) {
    rc = xml_msg_load_itti_s1ap_ue_context_release_req(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_S1AP_UE_CONTEXT_RELEASE_COMPLETE_XML_STR)) {
    rc = xml_msg_load_itti_s1ap_ue_context_release_complete(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_MME_APP_INITIAL_UE_MESSAGE_XML_STR)) {
    rc = xml_msg_load_itti_mme_app_initial_ue_message(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_MME_APP_INITIAL_CONTEXT_SETUP_RSP_XML_STR)) {
    rc = xml_msg_load_itti_mme_app_initial_context_setup_rsp(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF_XML_STR)) {
    rc = xml_msg_load_itti_mme_app_connection_establishment_cnf(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_UPLINK_DATA_IND_XML_STR)) {
    rc = xml_msg_load_itti_nas_uplink_data_ind(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_DOWNLINK_DATA_REQ_XML_STR)) {
    rc = xml_msg_load_itti_nas_downlink_data_req(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_DOWNLINK_DATA_REJ_XML_STR)) {
    rc = xml_msg_load_itti_nas_downlink_data_rej(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_DOWNLINK_DATA_CNF_XML_STR)) {
    rc = xml_msg_load_itti_nas_downlink_data_cnf(scenario, &scenario_player_item->u.msg);
  }
  return rc;
}

//------------------------------------------------------------------------------
int msp_reload_message (scenario_t * const scenario, scenario_player_item_t * const scenario_player_item)
{
  xmlNodePtr  cur = NULL;
  int         rc = RETURNerror;

  OAILOG_DEBUG (LOG_MME_SCENARIO_PLAYER, "Reloading message from XML doc\n");

  AssertFatal(scenario_player_item->u.msg.xml_doc, "No XML doc found!");

  cur = xmlDocGetRootElement(scenario_player_item->u.msg.xml_doc);
  AssertFatal (cur, "Could not get root element of XML doc");
  // better to clean instead of reset ?
  xmlXPathFreeContext(scenario_player_item->u.msg.xpath_ctx);
  scenario_player_item->u.msg.xpath_ctx = xmlXPathNewContext(scenario_player_item->u.msg.xml_doc);

  // free it (may be called from msp_reload_message)
  if (scenario_player_item->u.msg.itti_msg) {
    itti_free_msg_content (scenario_player_item->u.msg.itti_msg);
    itti_free (ITTI_MSG_ORIGIN_ID (scenario_player_item->u.msg.itti_msg), scenario_player_item->u.msg.itti_msg);
    scenario_player_item->u.msg.itti_msg = NULL;
  }

  while (scenario_player_item->u.msg.vars_to_load) {
    struct load_list_item_s *vars_to_load = scenario_player_item->u.msg.vars_to_load;
    scenario_player_item->u.msg.vars_to_load = vars_to_load->next;
    free_wrapper((void**)&vars_to_load);
  }

  if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_SCTP_NEW_ASSOCIATION_XML_STR)) {
    rc = xml_msg_load_itti_sctp_new_association(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_SCTP_CLOSE_ASSOCIATION_XML_STR)) {
    rc = xml_msg_load_itti_sctp_close_association(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_S1AP_UE_CONTEXT_RELEASE_REQ_XML_STR)) {
    rc = xml_msg_load_itti_s1ap_ue_context_release_req(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_S1AP_UE_CONTEXT_RELEASE_COMPLETE_XML_STR)) {
    rc = xml_msg_load_itti_s1ap_ue_context_release_complete(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_MME_APP_INITIAL_UE_MESSAGE_XML_STR)) {
    rc = xml_msg_load_itti_mme_app_initial_ue_message(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_MME_APP_INITIAL_CONTEXT_SETUP_RSP_XML_STR)) {
    rc = xml_msg_load_itti_mme_app_initial_context_setup_rsp(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_MME_APP_CONNECTION_ESTABLISHMENT_CNF_XML_STR)) {
    rc = xml_msg_load_itti_mme_app_connection_establishment_cnf(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_UPLINK_DATA_IND_XML_STR)) {
    rc = xml_msg_load_itti_nas_uplink_data_ind(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_DOWNLINK_DATA_REQ_XML_STR)) {
    rc = xml_msg_load_itti_nas_downlink_data_req(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_DOWNLINK_DATA_REJ_XML_STR)) {
    rc = xml_msg_load_itti_nas_downlink_data_rej(scenario, &scenario_player_item->u.msg);
  } else if (!xmlStrcmp(cur->name, (const xmlChar *) ITTI_NAS_DOWNLINK_DATA_CNF_XML_STR)) {
    rc = xml_msg_load_itti_nas_downlink_data_cnf(scenario, &scenario_player_item->u.msg);
  }
  return rc;
}

//------------------------------------------------------------------------------
void msp_free_message_content (scenario_player_msg_t * msg)
{
  if (msg) {
    if (msg->xml_doc) {
      xmlFreeDoc(msg->xml_doc);
      msg->xml_doc = NULL;
    }
    if (msg->xpath_ctx) {
      xmlXPathFreeContext(msg->xpath_ctx);
      msg->xpath_ctx = NULL;
    }

    if (msg->itti_msg) {
      //TODO  need to do more than that
      itti_free (ITTI_MSG_ORIGIN_ID (msg->itti_msg), msg->itti_msg);
      msg->itti_msg = NULL;
    }
  }
}

//------------------------------------------------------------------------------
void msp_scenario_add_item(scenario_t * const scenario, scenario_player_item_t * const item)
{
  if ((scenario) && (item)) {
    // add it in hashtable
    // test if key exist before because insert will overwrite entry and free it if item is var -> memory corruption
    hashtable_rc_t rc = hashtable_ts_is_key_exists (scenario->scenario_items, (hash_key_t)item->uid);
    if (HASH_TABLE_OK == rc) {
      AssertFatal(SCENARIO_PLAYER_ITEM_VAR == item->item_type, "Error duplicate item in scenario, allowed only for vars, item_type=%d", item->item_type);
    } else {
      hashtable_rc_t rc = hashtable_ts_insert (scenario->scenario_items, (hash_key_t)item->uid, (void *)item);
      AssertFatal(HASH_TABLE_OK == rc, "Error in putting item in hashtable");
    }

    // add it in list
    if (!scenario->head_item) {
      scenario->head_item = item;
    }

    if (scenario->tail_item) {
      scenario->tail_item->next_item = item;
    }
    item->next_item = NULL;
    item->previous_item = scenario->tail_item;
    scenario->tail_item = item;
    OAILOG_DEBUG (LOG_MME_SCENARIO_PLAYER, "Scenario added item type %d \n", item->item_type);
  }
}
//------------------------------------------------------------------------------
void msp_scenario_player_add_scenario(scenario_player_t * const scenario_player, scenario_t * const scenario)
{
  if ((scenario_player) && (scenario)) {

    if (scenario_player->tail_scenarios) {
      scenario_player->tail_scenarios->next_scenario = scenario;

      scenario->previous_scenario = scenario_player->tail_scenarios;

    } else {
      scenario_player->head_scenarios = scenario;
      scenario->previous_scenario = NULL;
    }
    scenario_player->tail_scenarios = scenario;
    scenario->next_scenario = NULL;
  }
}
//------------------------------------------------------------------------------
int msp_load_scenario (bstring file_path, scenario_player_t * const scenario_player)
{
  xmlDocPtr               doc = NULL;
  xmlNodePtr              cur = NULL;
  scenario_player_item_t *scenario_player_item = NULL;

  OAILOG_DEBUG (LOG_MME_SCENARIO_PLAYER, "Loading scenario from XML file %s\n", bdata(file_path));
  doc = xmlParseFile(bdata(file_path));
  if (doc == NULL ) {
    OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Document %s not parsed successfully\n", bdata(file_path));
    return RETURNerror;
  }
  cur = xmlDocGetRootElement(doc);
  if (cur == NULL) {
    // do not consider this as an error
    OAILOG_WARNING (LOG_MME_SCENARIO_PLAYER, "empty document %s \n", bdata(file_path));
    xmlFreeDoc(doc);
    return RETURNok;
  }

  if (xmlStrcmp(cur->name, (const xmlChar *) SCENARIO_NODE_XML_STR)) {
    OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "document %s of the wrong type, root node != %s %s\n", bdata(file_path), SCENARIO_NODE_XML_STR, cur->name);
    xmlFreeDoc(doc);
    return RETURNerror;
  }

  scenario_t * scenario = calloc(1, sizeof(scenario_t));
  msp_init_scenario(scenario);
  scenario_set_status(scenario, SCENARIO_STATUS_LOADING);

  xmlChar *attr = xmlGetProp(cur, (const xmlChar *)NAME_ATTR_XML_STR);
  if (attr) {
    OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found scenario %s %s\n", NAME_ATTR_XML_STR, attr);
    scenario->name = bfromcstr((const char *)attr);
    xmlFree(attr);
  } else {
    AssertFatal(0, "Could not find scenario %s", NAME_ATTR_XML_STR);
  }

  xmlXPathContextPtr xpath_ctx = xmlXPathNewContext(doc);
  bool res = true;
  bstring xpath_expr = bformat("/%s/*",SCENARIO_NODE_XML_STR);
  xmlXPathObjectPtr xpath_obj = xml_find_nodes(doc, &xpath_ctx, xpath_expr);
  if (xpath_obj) {
    xmlNodeSetPtr nodes = xpath_obj->nodesetval;
    int size = (nodes) ? nodes->nodeNr : 0;
    for (int item = 0 ; item < size; item ++) {
      xmlNodePtr saved_node_ptr = xpath_ctx->node;
      res = (RETURNok == xmlXPathSetContextNode(nodes->nodeTab[item], xpath_ctx));
      if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)MESSAGE_FILE_NODE_XML_STR))) {
        if ((scenario_player_item = msp_load_message_file(scenario, doc, xpath_ctx, nodes->nodeTab[item], file_path))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", MESSAGE_FILE_NODE_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)EXIT_ATTR_XML_STR))) {
        if ((scenario_player_item = msp_load_exit(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", EXIT_ATTR_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)LABEL_ATTR_XML_STR))) {
        if ((scenario_player_item = msp_load_label(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", LABEL_ATTR_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)VAR_NODE_XML_STR))) {
        if ((scenario_player_item = msp_load_var(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", VAR_NODE_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)INCR_VAR_INC_NODE_XML_STR))) {
        if ((scenario_player_item = msp_load_incr_var(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", INCR_VAR_INC_NODE_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)DECR_VAR_INC_NODE_XML_STR))) {
        if ((scenario_player_item = msp_load_decr_var(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", DECR_VAR_INC_NODE_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)JUMP_ON_COND_XML_STR))) {
        if ((scenario_player_item = msp_load_jcond(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", JUMP_ON_COND_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)SLEEP_NODE_XML_STR))) {
        if ((scenario_player_item = msp_load_sleep(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          msp_scenario_add_item(scenario, scenario_player_item);
        } else {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s\n", SLEEP_NODE_XML_STR);
        }
      } else if ((!xmlStrcmp(nodes->nodeTab[item]->name, (const xmlChar *)UE_SECURITY_CONTEXT_XML_STR))) {
        // not a scenario_player_item
        if ((msp_load_ue_security_context(scenario, doc, xpath_ctx, nodes->nodeTab[item]))) {
          scenario_set_status(scenario, SCENARIO_STATUS_LOAD_FAILED);
          OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Failed to load %s \n", UE_SECURITY_CONTEXT_XML_STR);
        }
      } else {
        OAILOG_TRACE (LOG_MME_SCENARIO_PLAYER, "Found %s\n", nodes->nodeTab[item]->name);
      }
      if (saved_node_ptr) {
        res = (RETURNok == xmlXPathSetContextNode(saved_node_ptr, xpath_ctx)) & res;
      }
    }
  }
  AssertFatal(SCENARIO_STATUS_LOADING == scenario->status, "Failed to load scenario %s\n", bdata(file_path));
  if (SCENARIO_STATUS_LOADING == scenario->status) {
    scenario_set_status(scenario, SCENARIO_STATUS_LOADED);
  } //else {
    //OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Scenario %s not loaded \n", bdata(file_path));
  //}
  msp_scenario_player_add_scenario(scenario_player, scenario);
  xmlFreeDoc(doc);
  return RETURNok;
}


//------------------------------------------------------------------------------
void msp_free_scenario_player_item (scenario_player_item_t * item)
{
  if (item) {
    switch (item->item_type) {
    case SCENARIO_PLAYER_ITEM_ITTI_MSG:
      msp_free_message_content(&item->u.msg);
      break;

    case SCENARIO_PLAYER_ITEM_LABEL:
      bdestroy_wrapper (&item->u.label);
      break;

    case SCENARIO_PLAYER_ITEM_VAR:
      if (VAR_VALUE_TYPE_BSTR == item->u.var.value_type) {
        bdestroy_wrapper(&item->u.var.value.value_bstr);
      }
      break;

    case SCENARIO_PLAYER_ITEM_EXIT:
    case SCENARIO_PLAYER_ITEM_SLEEP:
    case SCENARIO_PLAYER_ITEM_VAR_INCR:
    case SCENARIO_PLAYER_ITEM_VAR_DECR:
    case SCENARIO_PLAYER_ITEM_JUMP_COND:
      //NOP
      break;

    default:
      OAILOG_WARNING (LOG_MME_SCENARIO_PLAYER, "Unknown item type %d\n", item->item_type);
    }
    free(item);
  }
}
//------------------------------------------------------------------------------
void msp_free_scenario (scenario_t * scenario)
{
  if (scenario) {
    OAILOG_DEBUG (LOG_MME_SCENARIO_PLAYER, "Freeing scenario %s\n", bdata(scenario->name));
    pthread_mutex_lock(&scenario->lock);
    scenario_player_item_t *item = scenario->head_item;
    while (item) {
      scenario_player_item_t *last_item = item;
      item = item->next_item;
      msp_free_scenario_player_item(last_item);
    }
    xmlFreeDoc(scenario->xml_doc);
    bdestroy_wrapper (&scenario->name);
    hashtable_ts_destroy(scenario->scenario_items);
    obj_hashtable_ts_destroy(scenario->var_items);
    free_wrapper((void**)&scenario->ue_emulated_emm_security_context);

    pthread_mutex_unlock(&scenario->lock);
    pthread_mutex_destroy(&scenario->lock);
    free_wrapper((void**)&scenario);
  }
}

//------------------------------------------------------------------------------
int msp_load_scenarios (bstring file_path, scenario_player_t * const scenario_player)
{
  xmlDocPtr  doc = NULL;
  xmlNodePtr cur = NULL;

  OAILOG_DEBUG (LOG_MME_SCENARIO_PLAYER, "Loading scenarios from XML file %s\n", bdata(file_path));
  doc = xmlParseFile(bdata(file_path));
  if (doc == NULL ) {
    OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Document %s not parsed successfully\n", bdata(file_path));
    return RETURNerror;
  }
  cur = xmlDocGetRootElement(doc);
  if (cur == NULL) {
    // do not consider this as an error
    OAILOG_WARNING (LOG_MME_SCENARIO_PLAYER, "empty document %s \n", bdata(file_path));
    xmlFreeDoc(doc);
    return RETURNok;
  }

  if (xmlStrcmp(cur->name, (const xmlChar *) SCENARIOS_NODE_XML_STR)) {
    OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "document %s of the wrong type, root node != %s %s\n", bdata(file_path), SCENARIOS_NODE_XML_STR, cur->name);
    xmlFreeDoc(doc);
    return RETURNerror;
  }

  cur = cur->xmlChildrenNode;
  while (cur != NULL) {
    if ((!xmlStrcmp(cur->name, (const xmlChar *)SCENARIOS_FILE_NODE_XML_STR))) {
      xmlChar* scenarios_file = msp_get_attr_file (doc, cur);
      if (scenarios_file) {
        // remove file name from file_path
        bstring bstr = NULL;
        if ('/' == scenarios_file[0]) {
          // Absolute path
          bstr = bfromcstr ((const char *)scenarios_file);
          xmlFree(scenarios_file);
          int ret = msp_load_scenarios(bstr, scenario_player);
          bdestroy_wrapper (&bstr);
          if (0 > ret) {
            return RETURNerror;
          }
        } else {
          // relative path
          int pos = bstrrchrp (file_path, '/', blength(file_path) - 1);
          if (BSTR_ERR != pos) {
            bstr = bstrcpy(file_path);
            btrunc(bstr, pos + 1); // keep '/'
            bcatblk(bstr, scenarios_file, xmlStrlen(scenarios_file));
            xmlFree(scenarios_file);
            int ret = msp_load_scenarios(bstr, scenario_player);
            bdestroy_wrapper (&bstr);
            if (0 > ret) {
              return RETURNerror;
            }
          } else {
            OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Could not find char / in scenarios path %s\n", file_path->data);
            xmlFree(scenarios_file);
            return RETURNerror;
          }
        }
      } else {
        return RETURNerror;
      }
    } else if ((!xmlStrcmp(cur->name, (const xmlChar *)SCENARIO_FILE_NODE_XML_STR))){
      xmlChar* scenario_file = msp_get_attr_file (doc, cur);
      if (scenario_file) {
        // remove file name from file_path
        bstring bstr = NULL;
        if ('/' == scenario_file[0]) {
          // Absolute path
          bstr = bfromcstr ((const char *)scenario_file);
          xmlFree(scenario_file);
          int ret = msp_load_scenario(bstr, scenario_player);
          bdestroy_wrapper (&bstr);
          if (0 > ret) {
            return RETURNerror;
          }
        } else {
          // relative path
          int pos = bstrrchrp (file_path, '/', blength(file_path) - 1);
          if (BSTR_ERR != pos) {
            bstr = bstrcpy(file_path);
            btrunc(bstr, pos + 1); // keep '/'
            bcatblk(bstr, scenario_file, xmlStrlen(scenario_file)+1);
            xmlFree(scenario_file);
            int ret = msp_load_scenario(bstr, scenario_player);
            bdestroy_wrapper (&bstr);
            if (0 > ret) {
              return RETURNerror;
            }
          } else {
            OAILOG_ERROR (LOG_MME_SCENARIO_PLAYER, "Could not find char / in scenario path %s\n", file_path->data);
            xmlFree(scenario_file);
            return RETURNerror;
          }
        }
      } else {
        return RETURNerror;
      }
    }
    cur = cur->next;
  }
  xmlFreeDoc(doc);
  return RETURNok;
}

//------------------------------------------------------------------------------
void msp_fsm(void)
{
}
