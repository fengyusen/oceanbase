/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX RS
#include "ob_recovery_ls_service.h"

#include "lib/thread/threads.h"               //set_run_wrapper
#include "lib/mysqlclient/ob_mysql_transaction.h"  //ObMySQLTransaction
#include "lib/profile/ob_trace_id.h"
#include "logservice/ob_log_base_header.h"          //ObLogBaseHeader
#include "logservice/ob_log_handler.h"              //ObLogHandler
#include "logservice/palf/log_entry.h"              //LogEntry
#include "logservice/palf/log_define.h"
#include "share/scn.h"//SCN
#include "logservice/ob_garbage_collector.h"//ObGCLSLog
#include "logservice/restoreservice/ob_log_restore_handler.h"//ObLogRestoreHandler
#include "observer/ob_server_struct.h"              //GCTX
#include "rootserver/ob_primary_ls_service.h"       //ObTenantLSInfo
#include "rootserver/ob_tenant_recovery_reportor.h" //ObTenantRecoveryReportor
#include "rootserver/ob_tenant_info_loader.h" // ObTenantInfoLoader
#include "rootserver/ob_create_standby_from_net_actor.h" // ObCreateStandbyFromNetActor
#include "share/ls/ob_ls_life_manager.h"            //ObLSLifeManger
#include "share/ls/ob_ls_operator.h"                //ObLSAttr
#include "share/ls/ob_ls_recovery_stat_operator.h"  //ObLSRecoveryLSStatOperator
#include "share/ob_errno.h"
#include "share/ob_share_util.h"                           //ObShareUtil
#include "share/schema/ob_multi_version_schema_service.h"  //ObMultiSchemaService
#include "share/restore/ob_physical_restore_info.h" //restore_status
#include "share/restore/ob_physical_restore_table_operator.h"//ObPhysicalRestoreTableOperator
#include "share/ob_primary_standby_service.h" // ObPrimaryStandbyService
#include "share/ob_standby_upgrade.h"  // ObStandbyUpgrade
#include "share/ob_upgrade_utils.h"  // ObUpgradeChecker
#include "share/ob_global_stat_proxy.h" // ObGlobalStatProxy
#include "storage/tx/ob_tx_log.h"                          //ObTxLogHeader
#include "storage/tx_storage/ob_ls_service.h"              //ObLSService
#include "storage/tx_storage/ob_ls_handle.h"  //ObLSHandle
#include "src/rootserver/ob_rs_event_history_table_operator.h"

namespace oceanbase
{
using namespace logservice;
using namespace transaction;
using namespace share;
using namespace storage;
using namespace palf;
namespace rootserver
{

#define RESTORE_EVENT_ADD                                                      \
  int ret_code = OB_SUCCESS;                                                   \
  switch (ret) {                                                               \
    case -ER_ACCESS_DENIED_ERROR:                                              \
      ret_code = OB_PASSWORD_WRONG;                                            \
    case -ER_CONNECT_FAILED:                                                   \
      ret_code = OB_CONNECT_ERROR;                                             \
  }                                                                            \
  ROOTSERVICE_EVENT_ADD("root_service", "update_primary_ip_list",              \
    "tenant_id", tenant_id_, K(ret),                                           \
    "ob_error_name", ob_error_name(ret_code),                                  \
    "ob_error_str", ob_strerror(ret_code),                                     \
    "primary_user_tenant", user_and_tenant.ptr(),                              \
    "primary_ip_list", service_attr.addr_);                                    \

int ObRecoveryLSService::init()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("has inited", KR(ret));
  } else if (OB_FAIL(ObTenantThreadHelper::create("RecLSSer",
         lib::TGDefIDs::LSService, *this))) {
    LOG_WARN("failed to create thread", KR(ret));
  } else if (OB_FAIL(ObTenantThreadHelper::start())) {
    LOG_WARN("failed to start thread", KR(ret));
  } else {
    tenant_id_ = MTL_ID();
    proxy_ = GCTX.sql_proxy_;
    inited_ = true;
  }

  return ret;
}

void ObRecoveryLSService::destroy()
{
  LOG_INFO("recovery ls service destory", KPC(this));
  ObTenantThreadHelper::destroy();
  inited_ = false;
  tenant_id_ = OB_INVALID_TENANT_ID;
  proxy_ = NULL;
  restore_proxy_.destroy();
  primary_is_avaliable_= true;
}

void ObRecoveryLSService::do_work()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_) || OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(inited_), KP(proxy_));
  } else {
    ObLSRecoveryStatOperator ls_recovery;
    palf::PalfBufferIterator iterator;
    int tmp_ret = OB_SUCCESS;
    const int64_t idle_time_us = 100 * 1000L;
    SCN start_scn;
    while (!has_set_stop()) {
      ObCurTraceId::init(GCONF.self_addr_);
      uint64_t thread_idx = get_thread_idx();
      ObTenantInfoLoader *tenant_info_loader = MTL(ObTenantInfoLoader*);
      ObAllTenantInfo tenant_info;
      //two thread for seed log and recovery_ls_manager
      if (!is_user_tenant(tenant_id_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ls recovery thread must run on user tenant", KR(ret), K(tenant_id_));
      } else if (OB_ISNULL(tenant_info_loader)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tenant report is null", KR(ret), K(tenant_id_));
      } else if (OB_FAIL(tenant_info_loader->get_tenant_info(tenant_info))) {
        LOG_WARN("failed to get tenant info", KR(ret));
      } else if (OB_FAIL(check_can_do_recovery_(tenant_info))) {
        LOG_WARN("failed to check do recovery", KR(ret), K(tenant_info));
      } else if (0 == thread_idx) {
        if (OB_SUCCESS != (tmp_ret = process_recovery_ls_manager())) {
          ret = OB_SUCC(ret) ? tmp_ret : ret;
          LOG_WARN("failed to process recovery ls manager", KR(ret), KR(tmp_ret));
        }
        if (tenant_info.is_standby()) {
          if (REACH_TENANT_TIME_INTERVAL(10 * 1000 * 1000)) {
            (void)try_update_primary_ip_list();
          }
        }
      } else {
        if (!start_scn.is_valid()) {
          ObLSRecoveryStat ls_recovery_stat;
          if (OB_FAIL(ls_recovery.get_ls_recovery_stat(tenant_id_,
                  SYS_LS, false, ls_recovery_stat, *proxy_))) {
            LOG_WARN("failed to load sys recovery stat", KR(ret), K(tenant_id_));
          } else if (OB_FAIL(report_sys_ls_recovery_stat_(ls_recovery_stat.get_sync_scn()))) {
            //may recovery end, but readable scn need report
            LOG_WARN("failed to report ls recovery stat", KR(ret), K(ls_recovery_stat));
          } else if (tenant_info.get_recovery_until_scn() == ls_recovery_stat.get_sync_scn()) {
            ret = OB_EAGAIN;
            if (REACH_TIME_INTERVAL(60 * 1000 * 1000)) { // every minute
              LOG_INFO("has recovered to recovery_until_scn", KR(ret), K(ls_recovery_stat), K(tenant_info));
            }
          } else if (OB_FAIL(seek_log_iterator_(ls_recovery_stat.get_sync_scn(), iterator))) {
            LOG_WARN("failed to seek log iterator", KR(ret), K(ls_recovery_stat));
          } else {
            start_scn = ls_recovery_stat.get_sync_scn();
            LOG_INFO("start to seek at", K(start_scn));
          }
        }
        if (OB_FAIL(ret)) {
        } else if (start_scn.is_valid() && OB_FAIL(process_ls_log_(tenant_info, start_scn, iterator))) {
          if (OB_ITER_STOP != ret) {
            LOG_WARN("failed to process ls log", KR(ret), K(start_scn), K(tenant_info));
          }
        }
        if (OB_FAIL(ret)) {
          start_scn.reset();
        }

        if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) { // every 10 second
          (void)try_tenant_upgrade_end_();
        }
      }
      LOG_INFO("[LS_RECOVERY] finish one round", KR(ret), KR(tmp_ret), K(start_scn), K(thread_idx), K(tenant_info));
      idle(idle_time_us);
    }
  }
}

int ObRecoveryLSService::seek_log_iterator_(const SCN &sync_scn, PalfBufferIterator &iterator)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(inited_));
  } else if (OB_UNLIKELY(!sync_scn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("sync scn is invalid", KR(ret), K(sync_scn));
  } else {
    ObLSService *ls_svr = MTL(ObLSService *);
    ObLSHandle ls_handle;
    if (OB_FAIL(ls_svr->get_ls(SYS_LS, ls_handle, storage::ObLSGetMod::RS_MOD))) {
      LOG_WARN("failed to get ls", KR(ret));
    } else {
      ObLogHandler *log_handler = NULL;
      ObLS *ls = NULL;
      palf::LSN start_lsn(0);
      if (OB_ISNULL(ls = ls_handle.get_ls())
          || OB_ISNULL(log_handler = ls->get_log_handler())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ls or log handle is null", KR(ret), KP(ls),
                     KP(log_handler));
      } else {
        if (SCN::base_scn() == sync_scn) {
          // start_lsn = 0;
        } else {
          if (OB_FAIL(log_handler->locate_by_scn_coarsely(sync_scn, start_lsn))) {
            LOG_WARN("failed to locate lsn", KR(ret), K(sync_scn));
          }
        }

        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(log_handler->seek(start_lsn, iterator))) {
          LOG_WARN("failed to seek iterator", KR(ret), K(sync_scn), K(start_lsn));
        }
      }
    }
  }
  return ret;
}

int ObRecoveryLSService::process_ls_log_(
      const ObAllTenantInfo &tenant_info,
      SCN &start_scn,
      PalfBufferIterator &iterator)
{
  int ret = OB_SUCCESS;
  palf::LogEntry log_entry;
  palf::LSN target_lsn;
  SCN sync_scn;
  SCN last_sync_scn;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(inited_));
  } else if (OB_UNLIKELY(!tenant_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant_info is invalid", KR(ret), K(tenant_info));
  }
  while (OB_SUCC(ret) && OB_SUCC(iterator.next())) {
    if (OB_FAIL(iterator.get_entry(log_entry, target_lsn))) {
      LOG_WARN("failed to get log", KR(ret), K(log_entry));
    } else if (OB_ISNULL(log_entry.get_data_buf())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("log entry is null", KR(ret));
    } else {
      LOG_DEBUG("get log", K(log_entry), K(target_lsn), K(start_scn));
      sync_scn = log_entry.get_scn();
      const char *log_buf = log_entry.get_data_buf();
      const int64_t log_length = log_entry.get_data_len();
      logservice::ObLogBaseHeader header;
      const int64_t HEADER_SIZE = header.get_serialize_size();
      int64_t log_pos = 0;
      if (OB_UNLIKELY(sync_scn <= start_scn)) {
        //通过scn定位的LSN是不准确的，可能会获取多余的数据，所以需要把小于等于sync_scn的过滤掉
        continue;
      } else if (tenant_info.get_recovery_until_scn() < sync_scn) {
        if (OB_FAIL(report_sys_ls_recovery_stat_(tenant_info.get_recovery_until_scn()))) {
          LOG_WARN("failed to report_sys_ls_recovery_stat_", KR(ret), K(sync_scn), K(tenant_info),
                   K(log_entry), K(target_lsn), K(start_scn));
        // SYS LS has recovered to the recovery_until_scn, need stop iterate SYS LS log and reset start_scn
        } else if (OB_FAIL(seek_log_iterator_(tenant_info.get_recovery_until_scn(), iterator))) {
          LOG_WARN("failed to seek log iterator", KR(ret), K(sync_scn), K(tenant_info),
                    K(log_entry), K(target_lsn), K(start_scn));
        } else {
          ret = OB_ITER_STOP;
          LOG_WARN("SYS LS has recovered to the recovery_until_scn, need stop iterate SYS LS log",
                   KR(ret), K(sync_scn), K(tenant_info), K(log_entry), K(target_lsn), K(start_scn));
          start_scn.reset();
        }
      } else if (OB_FAIL(header.deserialize(log_buf, HEADER_SIZE, log_pos))) {
        LOG_WARN("failed to deserialize", KR(ret), K(HEADER_SIZE));
      } else if (OB_UNLIKELY(log_pos >= log_length)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("log pos is not expected", KR(ret), K(log_pos), K(log_length));
      } else if (logservice::GC_LS_LOG_BASE_TYPE == header.get_log_type()) {
        ObGCLSLog gc_log;
        if (OB_FAIL(gc_log.deserialize(log_buf, log_length, log_pos))) {
          LOG_WARN("failed to deserialize gc log", KR(ret), K(log_length));
        } else if (ObGCLSLOGType::OFFLINE_LS == gc_log.get_log_type()) {
          //set sys ls offline
          if (OB_FAIL(process_gc_log_(gc_log, sync_scn))) {
            LOG_WARN("failed to process gc log", KR(ret), K(sync_scn));
          }
        }
        // nothing
      } else if (logservice::TRANS_SERVICE_LOG_BASE_TYPE == header.get_log_type()) {
        ObTxLogBlock tx_log_block;
        ObTxLogBlockHeader tx_block_header;
        if (OB_FAIL(tx_log_block.init(log_buf, log_length, log_pos, tx_block_header))) {
          LOG_WARN("failed to init tx log block", KR(ret), K(log_length));
        } else if (OB_FAIL(process_ls_tx_log_(tx_log_block, sync_scn))) {
          LOG_WARN("failed to process ls tx log", KR(ret), K(tx_log_block), K(sync_scn));
        }
      } else {}
      if (OB_SUCC(ret)) {
        last_sync_scn = sync_scn;
      } else if (last_sync_scn.is_valid()) {
        //if ls_operator can not process, need to report last sync scn
        int tmp_ret = OB_SUCCESS;
        if (OB_TMP_FAIL(report_sys_ls_recovery_stat_(last_sync_scn))) {
          LOG_WARN("failed to report ls recovery stat", KR(ret), KR(tmp_ret), K(last_sync_scn));
        }
      }
    }
  }//end for each log
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
    if (OB_FAIL(report_sys_ls_recovery_stat_(sync_scn))) {
      LOG_WARN("failed to report ls recovery stat", KR(ret), K(sync_scn), K(iterator));
    }
  } else if (OB_SUCC(ret)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("iterator must be end", KR(ret), K(iterator));
  } else {
    LOG_WARN("failed to get next log", KR(ret), K(iterator));
  }

  return ret;
}

int ObRecoveryLSService::process_ls_tx_log_(ObTxLogBlock &tx_log_block, const SCN &sync_scn)
{
  int ret = OB_SUCCESS;
  bool has_operation = false;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(inited_));
  }
  while (OB_SUCC(ret)) {
    transaction::ObTxLogHeader tx_header;
    bool contain_big_segment = false;
    if (OB_FAIL(tx_log_block.get_next_log(tx_header, nullptr, &contain_big_segment))) {
      if (OB_ITER_END == ret || contain_big_segment) {
        if (contain_big_segment) {
          LOG_INFO("skip process a big segment log", KR(ret), K(tx_log_block));
        }
        ret = OB_SUCCESS;
        break;
      } else {
        LOG_WARN("failed to get next log", KR(ret));
      }
    } else if (transaction::ObTxLogType::TX_COMMIT_LOG !=
               tx_header.get_tx_log_type()) {
      // nothing
    } else {
      ObTxCommitLogTempRef temp_ref;
      ObTxCommitLog commit_log(temp_ref);
      const int64_t COMMIT_SIZE = commit_log.get_serialize_size();
      //TODO commit log may too large
      if (OB_FAIL(tx_log_block.deserialize_log_body(commit_log))) {
        LOG_WARN("failed to deserialize", KR(ret));
      } else {
        const ObTxBufferNodeArray &source_data =
            commit_log.get_multi_source_data();
        for (int64_t i = 0; OB_SUCC(ret) && i < source_data.count(); ++i) {
          const ObTxBufferNode &node = source_data.at(i);
          if (ObTxDataSourceType::STANDBY_UPGRADE == node.get_data_source_type()) {
            if (OB_FAIL(process_upgrade_log_(node))) {
              LOG_WARN("failed to process_upgrade_log_", KR(ret), K(node));
            }
          } else if (ObTxDataSourceType::LS_TABLE != node.get_data_source_type()) {
            // nothing
          } else if (has_operation) {
            ret = OB_ERR_UNEXPECTED;
            LOG_ERROR("one clog has more than one operation", KR(ret), K(commit_log));
          } else {
            has_operation = true;
            ObLSAttr ls_attr;
            const int64_t LS_SIZE = ls_attr.get_serialize_size();
            int64_t pos = 0;
            if (OB_FAIL(ls_attr.deserialize(node.get_data_buf().ptr(), LS_SIZE,
                                            pos))) {
              LOG_WARN("failed to deserialize", KR(ret), K(node), K(LS_SIZE));
            } else if (OB_UNLIKELY(pos > LS_SIZE)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("failed to get ls attr", KR(ret), K(pos), K(LS_SIZE));
            } else {
              LOG_INFO("get ls operation", K(ls_attr), K(sync_scn));
              //TODO ls recovery is too fast for ls manager, so it maybe failed, while change ls status
              //consider how to retry
              if (OB_FAIL(process_ls_operator_(ls_attr,
                                               sync_scn))) {
                LOG_WARN("failed to process ls operator", KR(ret), K(ls_attr),
                         K(sync_scn));
              }
            }
          }
        }// end for
      }
    }
  }  // end while for each tx_log

  return ret;
}

int ObRecoveryLSService::process_upgrade_log_(const ObTxBufferNode &node)
{
  int ret = OB_SUCCESS;
  uint64_t standby_data_version = 0;

  if (!node.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("node is invalid", KR(ret), K(node));
  } else {
    ObStandbyUpgrade primary_data_version;
    int64_t pos = 0;
    if (OB_FAIL(primary_data_version.deserialize(node.get_data_buf().ptr(), node.get_data_buf().length(), pos))) {
      LOG_WARN("failed to deserialize", KR(ret), K(node), KPHEX(node.get_data_buf().ptr(), node.get_data_buf().length()));
    } else if (OB_UNLIKELY(pos > node.get_data_buf().length())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get primary_data_version", KR(ret), K(pos), K(node.get_data_buf().length()));
    } else {
      LOG_INFO("get primary_data_version", K(primary_data_version));
      if (!primary_data_version.is_valid()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("primary_data_version not valid", KR(ret), K(primary_data_version));
      } else if (OB_FAIL(ObUpgradeChecker::get_data_version_by_cluster_version(
                                           GET_MIN_CLUSTER_VERSION(),
                                           standby_data_version))) {
        LOG_WARN("failed to get_data_version_by_cluster_version", KR(ret), K(GET_MIN_CLUSTER_VERSION()));
      } else if (primary_data_version.get_data_version() > standby_data_version) {
        ret = OB_EAGAIN;
        if (REACH_TIME_INTERVAL(30 * 60 * 1000 * 1000)) { // 30min
          LOG_ERROR("standby version is not new enough to recover primary clog", KR(ret),
                   K(primary_data_version), K(standby_data_version));
        }
      }
    }
  }
  return ret;
}

int ObRecoveryLSService::get_min_data_version_(uint64_t &compatible)
{
  int ret = OB_SUCCESS;
  compatible = 0;
  if (OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("sql proxy is null", KR(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, result) {
      uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id_);
      ObSqlString sql;
      if (OB_FAIL(sql.assign_fmt("select value as compatible from %s where tenant_id = '%lu' and name = 'compatible' ",
                                 OB_TENANT_PARAMETER_TNAME, tenant_id_))) {
        LOG_WARN("fail to generate sql", KR(ret), K_(tenant_id));
      } else if (OB_FAIL(proxy_->read(result, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("read config from __tenant_parameter failed",
                KR(ret), K_(tenant_id), K(exec_tenant_id), K(sql));
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("config result is null", KR(ret), K_(tenant_id), K(exec_tenant_id), K(sql));
      } else if (OB_FAIL(result.get_result()->next())) {
        if (OB_ITER_END == ret) {
          LOG_WARN("compatible not set, it is net standby tenant", KR(ret), K_(tenant_id), K(exec_tenant_id), K(sql));
          ret = OB_SUCCESS;
          compatible = 0;
        } else {
          LOG_WARN("get result next failed", KR(ret), K_(tenant_id), K(exec_tenant_id), K(sql));
        }
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", KR(ret), K_(tenant_id), K(exec_tenant_id), K(sql));
      } else {
        ObString compatible_str;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result.get_result(), "compatible", compatible_str);

        if (OB_FAIL(ret)) {
          LOG_WARN("failed to get result", KR(ret), K_(tenant_id), K(exec_tenant_id), K(sql));
        } else if (OB_FAIL(ObClusterVersion::get_version(compatible_str, compatible))) {
          LOG_WARN("parse version failed", KR(ret), K(compatible_str));
        }
      }
    }
  }
  return ret;
}

void ObRecoveryLSService::try_tenant_upgrade_end_()
{
  int ret = OB_SUCCESS;
  uint64_t min_data_version = 0;
  obrpc::ObCommonRpcProxy *rs_rpc_proxy_ = GCTX.rs_rpc_proxy_;
  if (OB_ISNULL(proxy_) || !is_user_tenant(tenant_id_) || OB_ISNULL(rs_rpc_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("invalid argument", KR(ret), KP(proxy_), K_(tenant_id), KP(rs_rpc_proxy_));
  } else if (OB_FAIL(get_min_data_version_(min_data_version))) {
    LOG_WARN("fail to get min data version", KR(ret), K_(tenant_id));
  } else if (min_data_version == DATA_CURRENT_VERSION) {
    // already upgrade end
  } else {
    ObGlobalStatProxy proxy(*proxy_, tenant_id_);
    uint64_t target_data_version = 0;
    uint64_t current_data_version = 0;
    if (OB_FAIL(proxy.get_target_data_version(false /* for_update */, target_data_version))) {
      LOG_WARN("fail to get target data version", KR(ret), K_(tenant_id));
    } else if (OB_FAIL(proxy.get_current_data_version(current_data_version))) {
      LOG_WARN("fail to get current data version", KR(ret), K_(tenant_id));
    } else if (!(target_data_version == current_data_version
              && target_data_version != min_data_version
              && min_data_version <= DATA_CURRENT_VERSION)) {
      ret = EAGAIN;
      LOG_WARN("data_version not match, run upgrade end later",
               KR(ret), K_(tenant_id), K(target_data_version), K(current_data_version),
               K(DATA_CURRENT_VERSION), K(min_data_version));
    } else {
      HEAP_VAR(obrpc::ObAdminSetConfigItem, item) {
      ObSchemaGetterGuard guard;
      const ObSimpleTenantSchema *tenant = NULL;
      obrpc::ObAdminSetConfigArg arg;
      item.exec_tenant_id_ = OB_SYS_TENANT_ID;
      const int64_t timeout = GCONF.internal_sql_execute_timeout;
      int64_t pos = ObClusterVersion::print_version_str(
                    item.value_.ptr(), item.value_.capacity(),
                    current_data_version);
      if (pos <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("current_data_version is invalid",
                  KR(ret), K_(tenant_id), K(current_data_version));
      } else if (OB_FAIL(GSCHEMASERVICE.get_tenant_schema_guard(OB_SYS_TENANT_ID, guard))) {
        LOG_WARN("fail to get schema guard", KR(ret));
      } else if (OB_FAIL(guard.get_tenant_info(tenant_id_, tenant))) {
        LOG_WARN("fail to get tenant info", KR(ret), K_(tenant_id));
      } else if (OB_ISNULL(tenant)) {
        ret = OB_TENANT_NOT_EXIST;
        LOG_WARN("tenant not exist", KR(ret), K_(tenant_id));
      } else if (OB_FAIL(item.tenant_name_.assign(tenant->get_tenant_name()))) {
        LOG_WARN("fail to assign tenant name", KR(ret), K_(tenant_id));
      } else if (OB_FAIL(item.name_.assign("compatible"))) {
        LOG_WARN("fail to assign config name", KR(ret), K_(tenant_id));
      } else if (OB_FAIL(arg.items_.push_back(item))) {
        LOG_WARN("fail to push back item", KR(ret), K(item));
      } else if (OB_FAIL(rs_rpc_proxy_->timeout(timeout).admin_set_config(arg))) {
        LOG_WARN("fail to set config", KR(ret), K(arg), K(timeout));
      } else {
        LOG_INFO("upgrade end", KR(ret), K(arg), K(timeout), K_(tenant_id), K(target_data_version),
                 K(current_data_version), K(DATA_CURRENT_VERSION), K(min_data_version));
      }
      } // end HEAP_VAR
    }
  }
}

int ObRecoveryLSService::process_gc_log_(logservice::ObGCLSLog &gc_log, const SCN &sync_scn)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  ObLSLifeAgentManager ls_life_agent(*proxy_);
  if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is null", KR(ret));
   } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("failed to start trans", KR(ret), K(meta_tenant_id));
  } else if (OB_FAIL(ls_life_agent.set_ls_offline_in_trans(
            tenant_id_, SYS_LS, share::OB_LS_TENANT_DROPPING, sync_scn, share::NORMAL_SWITCHOVER_STATUS,
            trans))) {
    LOG_WARN("failed to set offline", KR(ret), K(tenant_id_), K(sync_scn));
  } else {
    ObLSRecoveryStatOperator ls_recovery;
    ObLSRecoveryStat ls_recovery_stat;
    if (OB_FAIL(construct_ls_recovery_stat(sync_scn, ls_recovery_stat))) {
      LOG_WARN("failed to construct ls recovery stat", KR(ret), K(sync_scn));
    } else if (OB_FAIL(ls_recovery.update_ls_recovery_stat_in_trans(
            ls_recovery_stat, trans))) {
      LOG_WARN("failed to update ls recovery stat", KR(ret), K(ls_recovery_stat));
    }
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      ret = OB_SUCC(ret) ? tmp_ret : ret;
      LOG_WARN("failed to end trans", KR(ret), K(tmp_ret));
    }
  }
  return ret;
}

int ObRecoveryLSService::construct_ls_recovery_stat(const SCN &sync_scn,
                                                    ObLSRecoveryStat &ls_stat)
{
  int ret = OB_SUCCESS;
  ls_stat.reset();
  SCN readable_scn;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(inited_));
  } else if (OB_FAIL(ObTenantRecoveryReportor::get_readable_scn(SYS_LS, readable_scn))) {
    LOG_WARN("failed to get readable scn", KR(ret));
  } else if (OB_FAIL(ls_stat.init_only_recovery_stat(
          tenant_id_, SYS_LS, sync_scn, readable_scn))) {
    LOG_WARN("failed to init ls recovery stat", KR(ret), K(tenant_id_),
             K(sync_scn), K(readable_scn));
  }
  return ret;
}

int ObRecoveryLSService::process_ls_operator_(const share::ObLSAttr &ls_attr, const SCN &sync_scn)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  if (OB_UNLIKELY(!ls_attr.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ls attr is invalid", KR(ret), K(ls_attr));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is null", KR(ret));
  } else if (OB_FAIL(check_valid_to_operator_ls_(ls_attr, sync_scn))) {
    LOG_WARN("failed to check valid to operator ls", KR(ret), K(sync_scn), K(ls_attr));
  } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("failed to start trans", KR(ret), K(meta_tenant_id));
  } else if (OB_FAIL(process_ls_operator_in_trans_(ls_attr, sync_scn, trans))) {
    LOG_WARN("failed to process ls operator in trans", KR(ret), K(ls_attr), K(sync_scn));
  }

  if (OB_SUCC(ret)) {
    ObLSRecoveryStat ls_recovery_stat;
    ObLSRecoveryStatOperator ls_recovery;
    if (OB_FAIL(construct_ls_recovery_stat(sync_scn, ls_recovery_stat))) {
      LOG_WARN("failed to construct ls recovery stat", KR(ret), K(sync_scn));
    } else if (OB_FAIL(ls_recovery.update_ls_recovery_stat_in_trans(
            ls_recovery_stat, trans))) {
      LOG_WARN("failed to update ls recovery stat", KR(ret), K(ls_recovery_stat));
    }
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      ret = OB_SUCC(ret) ? tmp_ret : ret;
      LOG_WARN("failed to end trans", KR(ret), K(tmp_ret));
    }
  }
  return ret;
}

int ObRecoveryLSService::check_valid_to_operator_ls_(
    const share::ObLSAttr &ls_attr, const SCN &sync_scn)
{
  int ret = OB_SUCCESS;
  bool has_user_ls = true;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(inited_));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is null", KR(ret));
  } else if (OB_UNLIKELY(!sync_scn.is_valid() || !ls_attr.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("syns scn is invalid", KR(ret), K(sync_scn), K(ls_attr));
  } else if (share::is_ls_tenant_drop_pre_op(ls_attr.get_ls_operatin_type())) {
    ret = OB_ITER_STOP;
    LOG_WARN("can not process ls operator after tenant dropping", K(ls_attr));
  } else if (share::is_ls_tenant_drop_op(ls_attr.get_ls_operatin_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls recovery must stop while pre tenant dropping", KR(ret), K(ls_attr));
  } else if (OB_FAIL(ObCreateStandbyFromNetActor::check_has_user_ls(tenant_id_, proxy_, has_user_ls))) {
    LOG_WARN("check_has_user_ls failed", KR(ret), K(tenant_id_));
  } else if (has_user_ls) {
    SCN user_scn;
    ObLSRecoveryStatOperator ls_recovery;
    if (OB_FAIL(ls_recovery.get_user_ls_sync_scn(tenant_id_, *proxy_, user_scn))) {
      LOG_WARN("failed to get user scn", KR(ret), K(tenant_id_));
    } else if (user_scn >= sync_scn) {
      //other ls has larger sync scn, ls can operator
    } else {
      ret = OB_NEED_WAIT;
      LOG_WARN("can not process ls operator, need wait other ls sync", KR(ret),
          K(user_scn), K(sync_scn));
    }
  }

  return ret;
}

int ObRecoveryLSService::process_ls_operator_in_trans_(
    const share::ObLSAttr &ls_attr, const SCN &sync_scn, ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  if (OB_UNLIKELY(!ls_attr.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ls attr is invalid", KR(ret), K(ls_attr));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy is null", KR(ret));
  } else {
    ObLSStatusOperator ls_operator;
    share::ObLSStatusInfo ls_status;
    ObLSLifeAgentManager ls_life_agent(*proxy_);
    if (OB_UNLIKELY(!ls_attr.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("ls attr is invalid", KR(ret), K(ls_attr));
    } else if (share::is_ls_create_pre_op(ls_attr.get_ls_operatin_type())) {
      //create new ls;
      if (OB_FAIL(create_new_ls_(ls_attr, sync_scn, trans))) {
        LOG_WARN("failed to create new ls", KR(ret), K(sync_scn), K(ls_attr));
      }
    } else if (share::is_ls_create_abort_op(ls_attr.get_ls_operatin_type())) {
      if (OB_FAIL(ls_life_agent.drop_ls_in_trans(tenant_id_, ls_attr.get_ls_id(), share::NORMAL_SWITCHOVER_STATUS, trans))) {
        LOG_WARN("failed to drop ls", KR(ret), K(tenant_id_), K(ls_attr));
      }
    } else if (share::is_ls_drop_end_op(ls_attr.get_ls_operatin_type())) {
      if (OB_FAIL(ls_life_agent.set_ls_offline_in_trans(tenant_id_, ls_attr.get_ls_id(),
              ls_attr.get_ls_status(), sync_scn, share::NORMAL_SWITCHOVER_STATUS, trans))) {
        LOG_WARN("failed to set offline", KR(ret), K(tenant_id_), K(ls_attr), K(sync_scn));
      }
    } else {
      ObLSStatus target_status = share::OB_LS_EMPTY;
      if (OB_FAIL(ls_operator.get_ls_status_info(tenant_id_, ls_attr.get_ls_id(),
              ls_status, trans))) {
        LOG_WARN("failed to get ls status", KR(ret), K(tenant_id_), K(ls_attr));
      } else if (ls_status.ls_is_creating()) {
        ret = OB_EAGAIN;
        LOG_WARN("ls not created, need wait", KR(ret), K(ls_status));
      } else if (share::is_ls_create_end_op(ls_attr.get_ls_operatin_type())) {
        // set ls to normal
        target_status = share::OB_LS_NORMAL;
      } else if (share::is_ls_tenant_drop_op(ls_attr.get_ls_operatin_type())) {
        target_status = share::OB_LS_TENANT_DROPPING;
      } else if (share::is_ls_drop_pre_op(ls_attr.get_ls_operatin_type())) {
        target_status = share::OB_LS_DROPPING;
      } else if (share::is_ls_tenant_drop_pre_op(ls_attr.get_ls_operatin_type())) {
        target_status = share::OB_LS_PRE_TENANT_DROPPING;
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected operation type", KR(ret), K(ls_attr));
      }
      if (FAILEDx(ls_operator.update_ls_status(tenant_id_, ls_attr.get_ls_id(),
              ls_status.status_, target_status,
              share::NORMAL_SWITCHOVER_STATUS, trans))) {
        LOG_WARN("failed to update ls status", KR(ret), K(tenant_id_), K(ls_attr),
            K(ls_status), K(target_status));
      }
      LOG_INFO("[LS_RECOVERY] update ls status", KR(ret), K(ls_attr), K(target_status));
    }
  }

  return ret;
}
int ObRecoveryLSService::create_new_ls_(const share::ObLSAttr &ls_attr,
                                        const SCN &sync_scn,
                                        common::ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  if (!share::is_ls_create_pre_op(ls_attr.get_ls_operatin_type())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ls not create pre operation", KR(ret), K(ls_attr));
  } else {
    //create new ls;
    DEBUG_SYNC(BEFORE_RECOVER_USER_LS);
    share::schema::ObSchemaGetterGuard schema_guard;
    const share::schema::ObTenantSchema *tenant_schema = NULL;
    if (OB_ISNULL(GCTX.schema_service_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error", KR(ret));
    } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(
                   OB_SYS_TENANT_ID, schema_guard))) {
      LOG_WARN("fail to get schema guard", KR(ret));
    } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id_, tenant_schema))) {
      LOG_WARN("failed to get tenant ids", KR(ret), K(tenant_id_));
    } else if (OB_ISNULL(tenant_schema)) {
      ret = OB_TENANT_NOT_EXIST;
      LOG_WARN("tenant not exist", KR(ret), K(tenant_id_));
    } else {
      ObTenantLSInfo tenant_stat(GCTX.sql_proxy_, tenant_schema, tenant_id_,
                                 GCTX.srv_rpc_proxy_, GCTX.lst_operator_);
      ObLSFlag ls_flag = ls_attr.get_ls_flag();
      if (OB_FAIL(tenant_stat.gather_stat(true))) {
        LOG_WARN("failed to gather stat", KR(ret));
      } else if (OB_FAIL(tenant_stat.create_new_ls_for_recovery(ls_attr.get_ls_id(),
              ls_attr.get_ls_group_id(), ls_attr.get_create_scn(), trans, ls_flag))) {
        LOG_WARN("failed to add new ls status info", KR(ret), K(ls_attr), K(sync_scn));
      }
    }
    LOG_INFO("[LS_RECOVERY] create new ls", KR(ret), K(ls_attr));
  }
  return ret;
}


int ObRecoveryLSService::process_recovery_ls_manager()
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;

  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(inited_));
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", KR(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(
                 OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("fail to get schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id_, tenant_schema))) {
    LOG_WARN("failed to get tenant ids", KR(ret), K(tenant_id_));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_TENANT_NOT_EXIST;
    LOG_WARN("tenant not exist", KR(ret), K(tenant_id_));
  } else {
    ObTenantLSInfo tenant_stat(GCTX.sql_proxy_, tenant_schema, tenant_id_,
                               GCTX.srv_rpc_proxy_, GCTX.lst_operator_);
    if (OB_FAIL(tenant_stat.process_ls_stats_for_recovery())) {
      LOG_WARN("failed to do process in standby", KR(ret), K(tenant_id_));
    }
  }
  return ret;
}

int ObRecoveryLSService::check_can_do_recovery_(const ObAllTenantInfo &tenant_info)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(inited_));
  } else if (OB_UNLIKELY(!tenant_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant_info is invalid", KR(ret), K(tenant_info));
  } else if (tenant_info.is_primary()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("primary tenant can not do recovery", KR(ret), K(tenant_info));
  } else if (tenant_info.is_standby()) {
    //standby can do recovery
  } else if (tenant_info.is_restore()) {
    //need to check success to create init ls
    share::ObPhysicalRestoreTableOperator restore_table_operator;
    share::ObPhysicalRestoreJob job_info;
    if (OB_ISNULL(proxy_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sql can't null", K(ret), K(proxy_));
    } else if (OB_FAIL(restore_table_operator.init(proxy_, tenant_id_))) {
      LOG_WARN("fail to init restore table operator", KR(ret), K(tenant_id_));
    } else if (OB_FAIL(restore_table_operator.get_job_by_tenant_id(tenant_id_,
                                                                job_info))) {
      LOG_WARN("fail to get restore job", K(ret), K(tenant_id_));
    } else if (job_info.is_valid_status_to_recovery()) {
      //can do recovery
    } else {
      ret = OB_NEED_WAIT;
      LOG_WARN("restore tenant not valid to recovery", KR(ret), K(job_info));
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected tenant role", KR(ret), K(tenant_info));
  }
  return ret;
}

int ObRecoveryLSService::report_sys_ls_recovery_stat_(const SCN &sync_scn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K(inited_));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql can't null", K(ret), K(proxy_));
  } else if (SCN::base_scn() != sync_scn) {
    ObLSRecoveryStatOperator ls_recovery;
    ObLSRecoveryStat ls_recovery_stat;
    if (OB_FAIL(construct_ls_recovery_stat(sync_scn, ls_recovery_stat))) {
      LOG_WARN("failed to construct ls recovery stat", KR(ret), K(sync_scn));
    } else if (OB_FAIL(ls_recovery.update_ls_recovery_stat(ls_recovery_stat,
            *proxy_))) {
      LOG_WARN("failed to update ls recovery stat", KR(ret),
          K(ls_recovery_stat));
    }
  }
  return ret;
}

void ObRecoveryLSService::try_update_primary_ip_list()
{
  int ret = OB_SUCCESS;
  ObLogRestoreSourceMgr restore_source_mgr;
  ObLogRestoreSourceItem item;
  common::ObArray<common::ObAddr> primary_addrs;
  ObSqlString standby_source_value;
  ObRestoreSourceServiceAttr service_attr;
  ObMySQLTransaction trans;
  ObSqlString user_and_tenant;
  int64_t primary_cluster_id;
  uint64_t primary_tenant_id;
  char passwd[OB_MAX_PASSWORD_LENGTH + 1] = { 0 }; //unencrypted password

  if (OB_FAIL(restore_source_mgr.init(tenant_id_, proxy_))) {
    LOG_WARN("fail to init restore_source_mgr", K_(tenant_id));
  } else if (OB_FAIL(restore_source_mgr.get_source(item))) {
    LOG_WARN("get source failed", K_(tenant_id));
  } else if (!check_need_update_ip_list_(item)) {
    LOG_INFO("there is no log restore source record or the log restore source type is not service" , K(item));
  } else if (OB_FAIL(get_restore_source_value_(item, standby_source_value))) {
    LOG_WARN("fail to get service standby log_restore source value", K(item));
  } else if (OB_FAIL(service_attr.parse_service_attr_from_str(standby_source_value))) {
    LOG_WARN("fail to parse service attr", K(item), K(standby_source_value));
  } else if (!service_attr.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("service attr is invalid", K(service_attr));
  } else if (OB_FAIL(service_attr.get_password(passwd, sizeof(passwd)))) {
    LOG_WARN("get servcie attr password failed", K(service_attr));
  } else if (OB_FAIL(service_attr.get_user_str_(user_and_tenant))) {
    LOG_WARN("get user str failed", K(service_attr.user_.user_name_), K(service_attr.user_.tenant_name_));
  } else if (!primary_is_avaliable_ && restore_proxy_.is_inited()) {
    LOG_WARN("primary is not avaliable, retry init restore proxy");
    restore_proxy_.destroy();
  } else if (!restore_proxy_.is_inited() && OB_FAIL(restore_proxy_.init(tenant_id_/*standby*/,
      service_attr.addr_,
      user_and_tenant.ptr(),
      passwd,
      service_attr.user_.mode_ == ObCompatibilityMode::MYSQL_MODE ? OB_SYS_DATABASE_NAME : OB_ORA_SYS_SCHEMA_NAME))) {
    LOG_WARN("restore_proxy_ fail to connect to primary", K_(tenant_id), K(service_attr.addr_), K(user_and_tenant));
  } else if (OB_FAIL(restore_proxy_.get_cluster_id(service_attr.user_.tenant_id_, primary_cluster_id))) {
    LOG_WARN("restore proxy fail to get primary cluster id", K(service_attr.user_.tenant_id_));
  } else if (OB_FAIL(restore_proxy_.get_tenant_id(service_attr.user_.tenant_name_, primary_tenant_id))) {
    LOG_WARN("restore proxy fail to get primary tenant id", K(service_attr.user_.tenant_name_));
  } else if (OB_FAIL(restore_proxy_.get_server_ip_list(service_attr.user_.tenant_id_, primary_addrs))) {
    LOG_WARN("restore proxy fail to get primary server ip list", K(service_attr.user_.tenant_id_));
  } else if (primary_addrs.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant ip list is empty", K(primary_addrs));
  } else if ((primary_cluster_id != service_attr.user_.cluster_id_) || (primary_tenant_id != service_attr.user_.tenant_id_)) {
    LOG_WARN("primary cluster id or tenant id has changed",
    K(primary_cluster_id), K(service_attr.user_.cluster_id_), K(primary_tenant_id), K(service_attr.user_.tenant_id_));
  } else {
    bool cur_primary_state = true;
    if (cur_primary_state != primary_is_avaliable_) {
      primary_is_avaliable_ = true;
      ROOTSERVICE_EVENT_ADD("root_service", "update_primary_ip_list",
        "tenant_id", tenant_id_,
        "info", "primary connection recovery",
        "primary_user_tenant", user_and_tenant.ptr(),
        "primary_ip_list", primary_addrs);
    }

    bool addr_is_same = false;
    addr_is_same = service_attr.compare_addr_(primary_addrs);
    if (!addr_is_same) {
      common::ObArray<common::ObAddr> tmp_addr;
      if (OB_FAIL(tmp_addr.assign(service_attr.addr_))) {
        LOG_WARN("fail to assign service attr addr", K(service_attr.addr_));
      } else {
        service_attr.addr_.reset();
        ARRAY_FOREACH_N(primary_addrs, idx, cnt) {
          if (OB_FAIL(service_attr.addr_.push_back(primary_addrs.at(idx)))) {
            LOG_WARN("fail to push back primary addr", K(primary_addrs.at(idx)));
          }
        }
        LOG_INFO("primary ip list has changed", K(addr_is_same), K(primary_addrs), K(service_attr.addr_));
        if (OB_FAIL(ret)) {
          LOG_WARN("fail to update primary ip list");
        } else if (OB_FAIL(do_update_restore_source_(service_attr, restore_source_mgr))) {
          LOG_WARN("fail to update restore source", K(service_attr));
        }
        ROOTSERVICE_EVENT_ADD("root_service", "update_primary_ip_list",
          "tenant_id", tenant_id_,
          "info", "do update primary ip list",
          "primary_user_tenant", user_and_tenant.ptr(),
          "new_primary_ip_list", primary_addrs,
          "old_primary_ip_list", tmp_addr);
      }
    }
  }
  if (OB_FAIL(ret)) {
    bool cur_primary_state = false;
    if (cur_primary_state != primary_is_avaliable_) {
      primary_is_avaliable_ = false;
      LOG_WARN("standby recovery ls service state changed");
      RESTORE_EVENT_ADD;
    }
  }
}

bool ObRecoveryLSService::check_need_update_ip_list_(ObLogRestoreSourceItem &item)
{
  return item.is_valid() && ObLogRestoreSourceType::SERVICE == item.type_;
}

int ObRecoveryLSService::get_restore_source_value_(ObLogRestoreSourceItem &item, ObSqlString &standby_source_value)
{
  int ret = OB_SUCCESS;
  ObSqlString value;
  if (!item.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("log restore source item is invalid");
  } else if (OB_FAIL(standby_source_value.assign(item.value_))) {
    LOG_WARN("fail to assign standby source value", K(item.value_));
  }
  return ret;
}

int ObRecoveryLSService::do_update_restore_source_(
  ObRestoreSourceServiceAttr &old_attr,
  ObLogRestoreSourceMgr &restore_source_mgr)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObLogRestoreSourceItem tmp_item;
  ObRestoreSourceServiceAttr tmp_service_attr;
  ObSqlString tmp_standby_source_value;
  char updated_value_str[OB_MAX_BACKUP_DEST_LENGTH + 1] = { 0 };

  if (OB_FAIL(old_attr.gen_service_attr_str(updated_value_str, sizeof(updated_value_str)))) {
    LOG_WARN("gen service_attr_str failed", K(updated_value_str), K(old_attr));
  } else if (OB_FAIL(trans.start(proxy_, gen_meta_tenant_id(tenant_id_)))) {
    LOG_WARN("fail to start trans", K_(tenant_id));
  } else if (OB_FAIL(restore_source_mgr.get_source_for_update(tmp_item, trans))) {
    LOG_WARN("fail to get log restore source when double check" , K(tmp_item));
  } else if (ObLogRestoreSourceType::SERVICE != tmp_item.type_) {
    ret = OB_EAGAIN;
    LOG_WARN("log restore source type is not service", K_(tenant_id), K(tmp_item.type_));
  } else if (OB_FAIL(get_restore_source_value_(tmp_item, tmp_standby_source_value))) {
    LOG_WARN("fail to get service standby log restore source value", K(tmp_item));
  } else if (OB_FAIL(tmp_service_attr.parse_service_attr_from_str(tmp_standby_source_value))) {
    LOG_WARN("fail to parse service attr", K(tmp_item), K(tmp_standby_source_value));
  } else if (!tmp_service_attr.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("service attr is invalid", K(tmp_service_attr));
  } else if (!(tmp_service_attr.user_ == old_attr.user_)) {
    ret = OB_EAGAIN;
    LOG_WARN("log restore source record may be modified", K(tmp_service_attr.user_), K(old_attr.user_));
  } else if (!(0 == STRCMP(tmp_service_attr.encrypt_passwd_, old_attr.encrypt_passwd_))) {
    ret = OB_EAGAIN;
    LOG_WARN("log restore source password may be modified", K_(tenant_id));
  } else if (OB_FAIL(update_source_inner_table_(updated_value_str, sizeof(updated_value_str), trans, tmp_item))) {
    LOG_WARN("fail to add service source", K(updated_value_str), K(tmp_item.until_scn_));
  }

  int temp_ret = OB_SUCCESS;
  if (trans.is_started()) {
    if (OB_SUCCESS != (temp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("trans end failed", "is_commit", OB_SUCCESS == ret, K(temp_ret));
      ret = (OB_SUCC(ret)) ? temp_ret : ret;
    }
  }
  return ret;
}

int ObRecoveryLSService::update_source_inner_table_(char *buf,
                                                    const int64_t buf_size,
                                                    ObMySQLTransaction &trans,
                                                    const ObLogRestoreSourceItem &item)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;

  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_size <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument when get source for update",K(buf_size));
  } else {
    uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id_);
    ObDMLSqlSplicer dml;
    ObSqlString sql;
    if (OB_FAIL(dml.add_pk_column(OB_STR_TENANT_ID, tenant_id_))) {
      LOG_WARN("failed to add column", K_(tenant_id), K(item));
    } else if (OB_FAIL(dml.add_pk_column(OB_STR_LOG_RESTORE_SOURCE_ID, item.id_))) {
      LOG_WARN("failed to add column", K_(tenant_id), K(item));
    } else if (OB_FAIL(dml.add_column(OB_STR_LOG_RESTORE_SOURCE_VALUE, buf))) {
      LOG_WARN("failed to add column", K_(tenant_id), K(item));
    } else if (OB_FAIL(dml.splice_update_sql(OB_ALL_LOG_RESTORE_SOURCE_TNAME, sql))) {
      LOG_WARN("fill update source value sql failed", K(item));
    } else if (OB_FAIL(trans.write(exec_tenant_id, sql.ptr(), affected_rows))) {
      LOG_WARN("failed to exec sql", K(sql), K_(tenant_id));
    }
  }
  return ret;
}

}//end of namespace rootserver
}//end of namespace oceanbase

