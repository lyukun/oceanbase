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

#include "ob_unit_manager.h"

#include <cmath>
#include <float.h>

#include "lib/string/ob_sql_string.h"
#include "lib/allocator/ob_mod_define.h"
#include "lib/time/ob_time_utility.h"
#include "lib/container/ob_array_iterator.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "lib/mysqlclient/ob_mysql_transaction.h"
#include "lib/utility/ob_tracepoint.h"
#include "share/ob_unit_getter.h"
#include "share/ob_debug_sync.h"
#include "share/ob_srv_rpc_proxy.h"
#include "share/config/ob_server_config.h"
#include "share/ob_schema_status_proxy.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_max_id_fetcher.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/ob_tenant_memstore_info_operator.h"
#include "storage/ob_file_system_router.h"
#include "observer/ob_server_struct.h"
#include "rootserver/ob_balance_info.h"
#include "rootserver/ob_zone_manager.h"
#include "rootserver/ob_rs_event_history_table_operator.h"
#include "rootserver/ob_unit_placement_strategy.h"
#include "rootserver/ob_rs_job_table_operator.h"
#include "rootserver/ob_root_service.h"
#include "rootserver/ob_root_balancer.h"
#include "storage/ob_file_system_router.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace common::hash;
using namespace share;
using namespace share::schema;
namespace rootserver
{

int ObUnitManager::ZoneUnitPtr::assign(const ZoneUnitPtr &other)
{
  int ret = OB_SUCCESS;
  zone_ = other.zone_;
  if (OB_FAIL(copy_assign(unit_ptrs_, other.unit_ptrs_))) {
    LOG_WARN("fail to assign unit_ptrs", K(ret));
  }
  return ret;
}

int ObUnitManager::ZoneUnitPtr::sort_by_unit_id_desc()
{
  UnitGroupIdCmp cmp;
  std::sort(unit_ptrs_.begin(), unit_ptrs_.end(), cmp);
  return cmp.get_ret();
}

int ObUnitManager::ZoneUnit::assign(const ZoneUnit &other)
{
  int ret = OB_SUCCESS;
  zone_ = other.zone_;
  if (OB_FAIL(copy_assign(unit_infos_, other.unit_infos_))) {
    LOG_WARN("failed to assign unit_infos_", K(ret));
  }
  return ret;
}
////////////////////////////////////////////////////////////////
double ObUnitManager::ObUnitLoad::get_demand(ObResourceType resource_type) const
{
  double ret = -1;
  switch (resource_type) {
    case RES_CPU:
      ret = unit_config_->min_cpu();
      break;
    case RES_MEM:
      ret = static_cast<double>(unit_config_->memory_size());
      break;
    case RES_LOG_DISK:
      ret = static_cast<double>(unit_config_->log_disk_size());
      break;
    default:
      ret = -1;
      break;
  }
  return ret;
}

// sort asc
bool ObUnitManager::ObUnitLoadOrder::operator()(const ObUnitLoad &left,
                                                const ObUnitLoad &right)
{
  bool less = false;
  double left_load = 0;
  double right_load = 0;
  if (OB_SUCCESS != ret_) {
  } else if (!left.is_valid() || !right.is_valid()) {
    ret_ = OB_INVALID_ARGUMENT;
    RS_LOG(WARN, "invalid argument", K(left), K(right), K_(ret));
  } else if (OB_SUCCESS != (ret_ = ObResourceUtils::calc_load(left, server_load_, weights_, weights_count_, left_load))) {
  } else if (OB_SUCCESS != (ret_ = ObResourceUtils::calc_load(right, server_load_, weights_, weights_count_, right_load))) {
  } else {
    less = left_load < right_load;
  }
  return less;
}

////////////////////////////////////////////////////////////////
const common::ObAddr& ObUnitManager::ObServerLoad::get_server() const
{
  return status_.server_;
}

double ObUnitManager::ObServerLoad::get_assigned(ObResourceType resource_type) const
{
  double ret = -1;
  switch (resource_type) {
    case RES_CPU:
      ret = sum_load_.min_cpu();
      break;
    case RES_MEM:
      ret = static_cast<double>(sum_load_.memory_size());
      break;
    case RES_LOG_DISK:
      ret = static_cast<double>(sum_load_.log_disk_size());
      break;
    default:
      ret = -1;
      break;
  }
  return ret;
}



double ObUnitManager::ObServerLoad::get_max_assigned(ObResourceType resource_type) const
{
  double ret = -1;
  switch (resource_type) {
    case RES_CPU:
      ret = sum_load_.max_cpu();
      break;
    case RES_MEM:
      ret = static_cast<double>(sum_load_.memory_size());
      break;
    case RES_LOG_DISK:
      ret = static_cast<double>(sum_load_.log_disk_size());
      break;
    default:
      ret = -1;
    break;
  }
  return ret;
}

double ObUnitManager::ObServerLoad::get_capacity(ObResourceType resource_type) const
{
  double ret = -1;
  switch (resource_type) {
    case RES_CPU:
      ret = status_.resource_info_.cpu_;
      break;
    case RES_MEM:
      ret = static_cast<double>(status_.resource_info_.mem_total_);
      break;
    case RES_LOG_DISK:
      ret = static_cast<double>(status_.resource_info_.log_disk_total_);
      break;
    default:
      ret = -1;
      break;
  }
  return ret;
}

int ObUnitManager::ObServerLoad::assign(const ObServerLoad &other)
{
  int ret = OB_SUCCESS;
  sum_load_ = other.sum_load_;
  status_ = other.status_;
  all_normal_unit_ = other.all_normal_unit_;
  if (OB_FAIL(copy_assign(unit_loads_, other.unit_loads_))) {
    LOG_WARN("failed to assign unit_loads_", K(ret));
  } else if (OB_FAIL(copy_assign(mark_delete_indexes_, other.mark_delete_indexes_))) {
    LOG_WARN("failed to assign mark_delete_indexes_",K(ret));
  }
  return ret;
}

bool ObUnitManager::ObServerLoad::is_valid() const
{
  // sum_load_ config_name is empty, don't check it
  return status_.is_valid();
}

void ObUnitManager::ObServerLoad::reset()
{
  sum_load_.reset();
  status_.reset();
  unit_loads_.reset();
  all_normal_unit_ = true;
  mark_delete_indexes_.reset();
}

int ObUnitManager::ObServerLoad::get_load(double *weights, int64_t weights_count, double &load) const
{
  int ret = common::OB_SUCCESS;
  if (!is_valid() || weights_count != RES_MAX) {
    ret = OB_INVALID_ARGUMENT;
    RS_LOG(WARN, "invalid server load", "server_load", *this, K(ret), K(weights_count));
  } else if (OB_FAIL(ObResourceUtils::calc_load(*this, weights, weights_count, load))) {
    RS_LOG(WARN, "failed to calc load");
  }
  return ret;
}

int ObUnitManager::ObServerLoad::get_load_if_add(
    double *weights, int64_t weights_count,
    const share::ObUnitConfig &config, double &load) const
{
  int ret = OB_SUCCESS;
  if (!is_valid() || !config.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    RS_LOG(WARN, "invalid server load", "server_load", *this, K(config), K(ret));
  } else {
    ObServerLoad new_load;
    new_load.sum_load_ = this->sum_load_;
    new_load.status_ = this->status_;
    new_load.sum_load_ += config;
    if (OB_FAIL(ObResourceUtils::calc_load(new_load, weights, weights_count, load))) {
      RS_LOG(WARN, "failed to calc load");
    }
  }
  return ret;
}

int ObUnitManager::ObServerLoad::build(const ObArray<ObUnitLoad> *unit_loads,
                                       const ObServerStatus &status)
{
  int ret = OB_SUCCESS;
  reset();
  // if not unit on server, unit_loads is NULL, so don't check unit_loads here
  if (!status.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid status", K(status), K(ret));
  } else if (OB_FAIL(ObUnitManager::calc_sum_load(unit_loads, sum_load_))) {
    LOG_WARN("calc_sum_load failed", KP(unit_loads), K(ret));
  } else {
    status_ = status;
    if (NULL == unit_loads) {
      // do nothing
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < unit_loads->count(); ++i) {
        if (!unit_loads->at(i).is_valid()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit_load is null", "unit_load", unit_loads->at(i), K(ret));
        } else if (unit_loads->at(i).unit_->migrate_from_server_ == status.server_
                   || ObUnit::UNIT_STATUS_ACTIVE != unit_loads->at(i).unit_->status_) {
          all_normal_unit_ = false;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(unit_loads_.assign(*unit_loads))) {
        LOG_WARN("assign failed", K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::ObServerLoad::mark_delete(const int64_t index)
{
  int ret = OB_SUCCESS;
  if (!is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server_load", "server_load", *this, K(ret));
  } else if (index < 0 || index >= unit_loads_.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid index", K(index), "unit_load count", unit_loads_.count(), K(ret));
  } else if (!unit_loads_.at(index).is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid unit_load", "unit_load", unit_loads_.at(index), K(ret));
  } else if (has_exist_in_array(mark_delete_indexes_, index)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit_load already marked deleted", K_(mark_delete_indexes), K(index), K(ret));
  } else if (OB_FAIL(mark_delete_indexes_.push_back(index))) {
    LOG_WARN("push_back failed", K(ret));
  } else {
    sum_load_ -= *unit_loads_.at(index).unit_config_;
  }
  return ret;
}

int ObUnitManager::ObServerLoad::add_load(const ObUnitLoad &load)
{
  int ret = OB_SUCCESS;
  if (!is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server_load", "server_load", *this, K(ret));
  } else if (!load.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid load", K(load), K(ret));
  } else {
    sum_load_ += *load.unit_config_;
    if (OB_FAIL(unit_loads_.push_back(load))) {
      LOG_WARN("push_back failed", K(ret));
    }
  }
  return ret;
}

// sorted desc
bool ObUnitManager::ObServerLoadOrder::operator()(const ObServerLoad *left,
                                                  const ObServerLoad *right)
{
  bool greater = false;
  double left_load = 0;
  double right_load = 0;
  if (OB_SUCCESS != ret_) {
  } else if (OB_ISNULL(left) || OB_ISNULL(right)
             || !left->is_valid() || !right->is_valid()) {
    ret_ = OB_INVALID_ARGUMENT;
    RS_LOG(WARN, "invalid argument", K(left), K(right), K_(ret));
  } else if (OB_SUCCESS != (ret_ = left->get_load(weights_, weights_count_, left_load))) {
  } else if (OB_SUCCESS != (ret_ = right->get_load(weights_, weights_count_, right_load))) {
  } else {
    greater = left_load > right_load;
  }
  return greater;
}
////////////////////////////////////////////////////////////////
ObUnitManager::ObUnitManager(ObServerManager &server_mgr, ObZoneManager &zone_mgr)
: inited_(false), loaded_(false), proxy_(NULL), server_config_(NULL),
    srv_rpc_proxy_(NULL), server_mgr_(server_mgr),
    zone_mgr_(zone_mgr), ut_operator_(), id_config_map_(),
    name_config_map_(), config_ref_count_map_(), config_pools_map_(),
    config_pools_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    config_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    id_pool_map_(), name_pool_map_(),
    pool_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    pool_unit_map_(),
    pool_unit_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    id_unit_map_(),
    allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    server_loads_(),
    load_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    tenant_pools_map_(),
    tenant_pools_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    server_migrate_units_map_(),
    migrate_units_allocator_(OB_MALLOC_NORMAL_BLOCK_SIZE, ObMalloc(ObModIds::OB_RS_UNIT_MANAGER)),
    lock_(ObLatchIds::UNIT_MANAGER_LOCK),
    schema_service_(NULL), root_balance_(NULL)
{
}

ObUnitManager::~ObUnitManager()
{
  ObHashMap<uint64_t, ObArray<share::ObResourcePool *> *>::iterator iter1;
  for (iter1 = config_pools_map_.begin(); iter1 != config_pools_map_.end(); ++iter1) {
    ObArray<share::ObResourcePool *> *ptr = iter1->second;
    if (NULL != ptr) {
      ptr->reset();
      ptr = NULL;
    }
  }
  ObHashMap<uint64_t, ObArray<ObUnit *> *>::iterator iter2;
  for (iter2 = pool_unit_map_.begin(); iter2 != pool_unit_map_.end(); ++iter2) {
    ObArray<share::ObUnit *> *ptr = iter2->second;
    if (NULL != ptr) {
      ptr->reset();
      ptr = NULL;
    }
  }
  ObHashMap<ObAddr, ObArray<ObUnitLoad> *>::iterator iter3;
  for (iter3 = server_loads_.begin(); iter3 != server_loads_.end(); ++iter3) {
    ObArray<ObUnitLoad> *ptr = iter3->second;
    if (NULL != ptr) {
      ptr->reset();
      ptr = NULL;
    }
  }
  TenantPoolsMap::iterator iter4;
  for (iter4 = tenant_pools_map_.begin(); iter4 != tenant_pools_map_.end(); ++iter4) {
    common::ObArray<share::ObResourcePool *> *ptr = iter4->second;
    if (NULL != ptr) {
      ptr->reset();
      ptr = NULL;
    }
  }
  ObHashMap<ObAddr, ObArray<uint64_t> *>::iterator iter5;
  for (iter5 = server_migrate_units_map_.begin();
       iter5 != server_migrate_units_map_.end();
       ++iter5) {
    common::ObArray<uint64_t> *ptr = iter5->second;
    if (NULL != ptr) {
      ptr->reset();
      ptr = NULL;
    }
  }
}

void ObUnitManager::dump()
{
  ObHashMap<ObAddr, ObArray<uint64_t> *>::iterator iter5;
  for (iter5 = server_migrate_units_map_.begin();
       iter5 != server_migrate_units_map_.end();
       ++iter5) {
    common::ObArray<uint64_t> *ptr = iter5->second;
    ObAddr server = iter5->first;
    if (OB_ISNULL(ptr)) {
      LOG_WARN("DUMP get invalid unit info", K(server));
    } else {
      LOG_INFO("DUMP SERVER_MIGRATE_UNIT_MAP", K(server), K(*ptr));
    }
  }
}

int ObUnitManager::init(ObMySQLProxy &proxy,
                        ObServerConfig &server_config,
                        obrpc::ObSrvRpcProxy &srv_rpc_proxy,
                        share::schema::ObMultiVersionSchemaService &schema_service,
                        ObRootBalancer &root_balance)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_FAIL(ut_operator_.init(proxy, NULL))) {
    LOG_WARN("init unit table operator failed", K(ret));
  } else if (OB_FAIL(pool_unit_map_.create(
              POOL_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_POOL_UNIT_MAP))) {
    LOG_WARN("pool_unit_map_ create failed", LITERAL_K(POOL_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(id_unit_map_.create(
              UNIT_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_ID_UNIT_MAP))) {
    LOG_WARN("id_unit_map_ create failed",
             LITERAL_K(UNIT_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(id_config_map_.create(
              CONFIG_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_ID_CONFIG_MAP))) {
    LOG_WARN("id_config_map_ create failed",
             LITERAL_K(CONFIG_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(name_config_map_.create(
              CONFIG_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_NAME_CONFIG_MAP))) {
    LOG_WARN("name_config_map_ create failed",
             LITERAL_K(CONFIG_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(config_ref_count_map_.create(
              CONFIG_REF_COUNT_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_CONFIG_REF_COUNT_MAP))) {
    LOG_WARN("config_ref_count_map_ create failed",
             LITERAL_K(CONFIG_REF_COUNT_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(config_pools_map_.create(CONFIG_POOLS_MAP_BUCKET_NUM,
                                              ObModIds::OB_HASH_BUCKET_CONFIG_POOLS_MAP))) {
    LOG_WARN("create config_pools_map failed",
             LITERAL_K(CONFIG_POOLS_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(id_pool_map_.create(
              POOL_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_ID_POOL_MAP))) {
    LOG_WARN("id_pool_map_ create failed",
             LITERAL_K(POOL_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(name_pool_map_.create(
              POOL_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_NAME_POOL_MAP))) {
    LOG_WARN("name_pool_map_ create failed",
             LITERAL_K(POOL_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(server_loads_.create(
              UNITLOAD_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_SERVER_UNITLOAD_MAP))) {
    LOG_WARN("server_loads_ create failed",
             LITERAL_K(UNITLOAD_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(tenant_pools_map_.create(
              TENANT_POOLS_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_TENANT_POOLS_MAP))) {
    LOG_WARN("tenant_pools_map_ create failed",
             LITERAL_K(TENANT_POOLS_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(server_migrate_units_map_.create(
              SERVER_MIGRATE_UNITS_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_SERVER_MIGRATE_UNIT_MAP))) {
    LOG_WARN("server_migrate_units_map_ create failed",
             LITERAL_K(SERVER_MIGRATE_UNITS_MAP_BUCKET_NUM), K(ret));
  } else {
    proxy_ = &proxy;
    server_config_ = &server_config;
    srv_rpc_proxy_ = &srv_rpc_proxy;
    schema_service_ = &schema_service;
    root_balance_ = &root_balance;
    loaded_ = false;
    inited_ = true;
  }
  return ret;
}

int ObUnitManager::load()
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    loaded_ = false;
    ObHashMap<uint64_t, ObArray<share::ObResourcePool *> *>::iterator iter1;
    for (iter1 = config_pools_map_.begin(); iter1 != config_pools_map_.end(); ++iter1) {
      ObArray<share::ObResourcePool *> *ptr = iter1->second;
      if (NULL != ptr) {
        ptr->reset();
        ptr = NULL;
      }
    }
    ObHashMap<uint64_t, ObArray<ObUnit *> *>::iterator iter2;
    for (iter2 = pool_unit_map_.begin(); iter2 != pool_unit_map_.end(); ++iter2) {
      ObArray<share::ObUnit *> *ptr = iter2->second;
      if (NULL != ptr) {
        ptr->reset();
        ptr = NULL;
      }
    }
    ObHashMap<ObAddr, ObArray<ObUnitLoad> *>::iterator iter3;
    for (iter3 = server_loads_.begin(); iter3 != server_loads_.end(); ++iter3) {
      ObArray<ObUnitLoad> *ptr = iter3->second;
      if (NULL != ptr) {
        ptr->reset();
        ptr = NULL;
      }
    }
    TenantPoolsMap::iterator iter4;
    for (iter4 = tenant_pools_map_.begin(); iter4 != tenant_pools_map_.end(); ++iter4) {
      common::ObArray<share::ObResourcePool *> *ptr = iter4->second;
      if (NULL != ptr) {
        ptr->reset();
        ptr = NULL;
      }
    }
    ObHashMap<ObAddr, ObArray<uint64_t> *>::iterator iter5;
    for (iter5 = server_migrate_units_map_.begin();
         iter5 != server_migrate_units_map_.end();
         ++iter5) {
      common::ObArray<uint64_t> *ptr = iter5->second;
      if (NULL != ptr) {
        ptr->reset();
        ptr = NULL;
      }
    }
    if (OB_FAIL(id_config_map_.clear())) {
      LOG_WARN("id_config_map_ clear failed", K(ret));
    } else if (OB_FAIL(name_config_map_.clear())) {
      LOG_WARN("name_pool_map_  clear failed", K(ret));
    } else if (OB_FAIL(config_ref_count_map_.clear())) {
      LOG_WARN("config_ref_count_map_ clear failed", K(ret));
    } else if (OB_FAIL(config_pools_map_.clear())) {
      LOG_WARN("config_pools_map_ clear failed", K(ret));
    } else if (OB_FAIL(id_pool_map_.clear())) {
      LOG_WARN("id_pool_map_ clear failed", K(ret));
    } else if (OB_FAIL(name_pool_map_.clear())) {
      LOG_WARN("name_pool_map_ clear failed", K(ret));
    } else if (OB_FAIL(pool_unit_map_.clear())) {
      LOG_WARN("pool_unit_map_ clear failed", K(ret));
    } else if (OB_FAIL(id_unit_map_.clear())) {
      LOG_WARN("id_unit_map_ clear failed", K(ret));
    } else if (OB_FAIL(server_loads_.clear())) {
      LOG_WARN("server_loads_ clear failed", K(ret));
    } else if (OB_FAIL(tenant_pools_map_.clear())) {
      LOG_WARN("tenant_pools_map_ clear failed", K(ret));
    } else if (OB_FAIL(server_migrate_units_map_.clear())) {
      LOG_WARN("server_migrate_units_map_ clear failed", K(ret));
    }

    // free all memory
    if (OB_SUCC(ret)) {
      config_allocator_.reset();
      config_pools_allocator_.reset();
      pool_allocator_.reset();
      pool_unit_allocator_.reset();
      allocator_.reset();
      load_allocator_.reset();
      tenant_pools_allocator_.reset();
      migrate_units_allocator_.reset();
    }

    // load unit config
    ObArray<ObUnitConfig> configs;
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ut_operator_.get_unit_configs(configs))) {
        LOG_WARN("get_unit_configs failed", K(ret));
      } else if (OB_FAIL(build_config_map(configs))) {
        LOG_WARN("build_config_map failed", K(ret));
      }
    }

    // load resource pool
    ObArray<share::ObResourcePool> pools;
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ut_operator_.get_resource_pools(pools))) {
        LOG_WARN("get_resource_pools failed", K(ret));
      } else if (OB_FAIL(build_pool_map(pools))) {
        LOG_WARN("build_pool_map failed", K(ret));
      }
    }

    // load unit
    ObArray<ObUnit> units;
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ut_operator_.get_units(units))) {
        LOG_WARN("get_units failed", K(ret));
      } else if (OB_FAIL(build_unit_map(units))) {
        LOG_WARN("build_unit_map failed", K(ret));
      }
    }

    // build tenant pools
    if (OB_SUCC(ret)) {
      for (ObHashMap<uint64_t, share::ObResourcePool *>::iterator it = id_pool_map_.begin();
           OB_SUCCESS == ret && it != id_pool_map_.end(); ++it) {
        // pool not grant to tenant don't add to tenant_pools_map
        if (NULL == it->second) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("it->second is null", KP(it->second), K(ret));
        } else if (it->second->is_granted_to_tenant()) {
          if (OB_FAIL(insert_tenant_pool(it->second->tenant_id_, it->second))) {
            LOG_WARN("insert_tenant_pool failed", "tenant_id", it->second->tenant_id_,
                     "pool", *(it->second), K(ret));
          }
        }
      }
    }

    // build server migrate units
    if (OB_SUCC(ret)) {
      FOREACH_CNT_X(unit, units, OB_SUCCESS == ret) {
        if (unit->migrate_from_server_.is_valid()) {
          if (OB_FAIL(insert_migrate_unit(unit->migrate_from_server_, unit->unit_id_))) {
            LOG_WARN("insert_migrate_unit failed", "server", unit->migrate_from_server_,
                     "unit_id", unit->unit_id_, K(ret));
          }
        }
      }
    }

    if (OB_SUCC(ret)) {
      loaded_ = true;
    }
  }
  LOG_INFO("unit manager load finish", K(ret));
  return ret;
}

int ObUnitManager::create_unit_config(const ObUnitConfig &unit_config, const bool if_not_exist)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (OB_FAIL(inner_create_unit_config_(unit_config, if_not_exist))) {
    LOG_WARN("fail to create unit config", KR(ret), K(unit_config), K(if_not_exist));
  }
  return ret;
}

int ObUnitManager::inner_create_unit_config_(const ObUnitConfig &unit_config, const bool if_not_exist)
{
  int ret = OB_SUCCESS;
  ObUnitConfig *temp_config = NULL;
  ObUnitConfig *new_config = NULL;
  uint64_t unit_config_id = unit_config.unit_config_id();
  const ObUnitConfigName &name = unit_config.name();
  const ObUnitResource &rpc_ur = unit_config.unit_resource();
  ObUnitResource ur = rpc_ur;

  LOG_INFO("start create unit config", K(name), K(rpc_ur), K(if_not_exist));

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (name.is_empty()) {
    ret = OB_MISS_ARGUMENT;
    LOG_WARN("miss 'name' argument", KR(ret), K(name));
    LOG_USER_ERROR(OB_MISS_ARGUMENT, "resource unit name");
  } else if (OB_FAIL(ur.init_and_check_valid_for_unit(rpc_ur))) {
    LOG_WARN("init from user specified unit resource and check valid fail", KR(ret), K(rpc_ur));
  } else if (OB_SUCCESS == (ret = get_unit_config_by_name(name, temp_config))) {
    if (NULL == temp_config) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("temp_config is null", KP(temp_config), K(ret));
    } else if (if_not_exist) {
      LOG_USER_NOTE(OB_RESOURCE_UNIT_EXIST, to_cstring(name));
      LOG_INFO("unit config already exist", K(name));
    } else {
      ret = OB_RESOURCE_UNIT_EXIST;
      LOG_USER_ERROR(OB_RESOURCE_UNIT_EXIST, to_cstring(name));
      LOG_WARN("unit config already exist", K(name), KR(ret));
    }
  } else if (OB_ENTRY_NOT_EXIST != ret) {
    LOG_WARN("get_unit_config_by_name failed", "config_name", name, KR(ret));
  }
  // allocate new unit config id
  else if (OB_INVALID_ID == unit_config_id && OB_FAIL(fetch_new_unit_config_id(unit_config_id))) {
    LOG_WARN("fetch_new_unit_config_id failed", KR(ret), K(unit_config));
  } else {
    if (OB_ISNULL(new_config = config_allocator_.alloc())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc memory failed", KR(ret));
    } else if (OB_FAIL(new_config->init(unit_config_id, name, ur))) {
      LOG_WARN("init unit config fail", KR(ret), K(unit_config_id), K(name), K(ur));
    } else if (OB_FAIL(ut_operator_.update_unit_config(*proxy_, *new_config))) {
      LOG_WARN("update_unit_config failed", "unit config", *new_config, K(ret));
    } else if (OB_FAIL(insert_unit_config(new_config))) {
      LOG_WARN("insert_unit_config failed", "unit config", *new_config,  K(ret));
    }

    // avoid memory leak
    if (OB_FAIL(ret) && NULL != new_config) {
      config_allocator_.free(new_config);
      new_config = NULL;
    }
  }
  LOG_INFO("finish create unit config", KR(ret), K(name), K(rpc_ur), K(if_not_exist), KPC(new_config));
  return ret;
}

int ObUnitManager::alter_unit_config(const ObUnitConfig &unit_config)
{
  int ret = OB_SUCCESS;
  ObUnitConfig *old_config = NULL;
  ObArray<share::ObResourcePool *> *pools = NULL;
  SpinWLockGuard guard(lock_);
  const ObUnitConfigName &name = unit_config.name();
  const ObUnitResource &rpc_ur = unit_config.unit_resource();

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (name.is_empty()) {
    ret = OB_MISS_ARGUMENT;
    LOG_WARN("miss 'name' argument", KR(ret));
    LOG_USER_ERROR(OB_MISS_ARGUMENT, "resource unit name");
  } else if (OB_FAIL(get_unit_config_by_name(name, old_config))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_unit_config_by_name failed", K(name), K(ret));
    } else {
      // overwrite ret on purpose
      ret = OB_RESOURCE_UNIT_NOT_EXIST;
      LOG_WARN("config does not exist", K(name), KR(ret));
      LOG_USER_ERROR(OB_RESOURCE_UNIT_NOT_EXIST, to_cstring(name));
    }
  } else if (OB_ISNULL(old_config)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("old_config is null", KP(old_config), KR(ret));
  } else {
    // copy unit resource
    ObUnitResource new_ur = old_config->unit_resource();
    const ObUnitResource &old_ur = old_config->unit_resource();

    // update based on user specified
    if (OB_FAIL(new_ur.update_and_check_valid_for_unit(rpc_ur))) {
      LOG_WARN("update and check valid for unit fail", KR(ret), K(new_ur), K(rpc_ur));
    } else {
      if (OB_FAIL(get_pools_by_config(old_config->unit_config_id(), pools))) {
        if (OB_ENTRY_NOT_EXIST != ret) {
          LOG_WARN("get_pools_by_config failed", "config_id", old_config->unit_config_id(), KR(ret));
        } else {
          ret = OB_SUCCESS;
          // this unit config is not used by any resource pools
          // update the config directly
        }
      } else if (NULL == pools) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pools is null", KP(pools), K(ret));
      } else {
        if (OB_FAIL(check_expand_resource_("ALTER_RESOURCE_UNIT", *pools, old_ur, new_ur))) {
          LOG_WARN("check expand config failed", K(old_ur), K(new_ur), KR(ret));
        } else if (OB_FAIL(check_shrink_resource_(*pools, old_ur, new_ur))) {
          LOG_WARN("check shrink config failed", K(old_ur), K(new_ur), KR(ret));
        } else if (OB_FAIL(check_full_resource_pool_memory_condition(*pools, new_ur.memory_size()))) {
          LOG_WARN("fail to check full resource pool memory condition", K(ret), K(new_ur), KPC(pools));
        }
      }
    }

    if (OB_SUCC(ret)) {
      ObUnitConfig new_config;
      if (OB_FAIL(new_config.init(old_config->unit_config_id(), old_config->name(), new_ur))) {
        LOG_WARN("init new unit config fail", KR(ret), KPC(old_config), K(new_ur));
      } else if (OB_FAIL(ut_operator_.update_unit_config(*proxy_, new_config))) {
        LOG_WARN("update_unit_config failed", K(new_config), KR(ret));
      } else if (OB_FAIL(old_config->update_unit_resource(new_ur))) {
        LOG_WARN("update unit resource of unit config fail", KR(ret), KPC(old_config), K(new_ur));
      }
    }

    if (OB_SUCC(ret)) {
      ROOTSERVICE_EVENT_ADD("unit", "alter_resource_unit",
                            "name", old_config->name(),
                            "unit_config_id", old_config->unit_config_id(),
                            "old_resource", old_ur,
                            "new_resource", new_ur);
    }
  }
  return ret;
}

int ObUnitManager::drop_unit_config(const ObUnitConfigName &name, const bool if_exist)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start drop unit config", K(name));
  SpinWLockGuard guard(lock_);
  ObUnitConfig *config = NULL;
  int64_t ref_count = 0;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource unit name");
    LOG_WARN("invalid argument", K(name), K(ret));
  } else if (OB_FAIL(get_unit_config_by_name(name, config))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_unit_config_by_name failed", K(name), K(ret));
    } else {
      if (if_exist) {
        ret = OB_SUCCESS;
        LOG_USER_NOTE(OB_RESOURCE_UNIT_NOT_EXIST, to_cstring(name));
        LOG_INFO("unit config not exist, no need to delete it", K(name));
      } else {
        ret = OB_RESOURCE_UNIT_NOT_EXIST;
        LOG_USER_ERROR(OB_RESOURCE_UNIT_NOT_EXIST, to_cstring(name));
        LOG_WARN("unit config not exist", K(name), K(ret));
      }
    }
  } else if (NULL == config) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("config is null", KP(config), K(ret));
  } else if (OB_FAIL(get_config_ref_count(config->unit_config_id(), ref_count))) {
    LOG_WARN("get_config_ref_count failed", "config_id", config->unit_config_id(), K(ret));
  } else if (0 != ref_count) {
    ret = OB_RESOURCE_UNIT_IS_REFERENCED;
    LOG_USER_ERROR(OB_RESOURCE_UNIT_IS_REFERENCED, to_cstring(name));
    LOG_WARN("some resource pool is using this unit config, can not delete it",
             K(ref_count), K(ret));
  } else if (OB_FAIL(ut_operator_.remove_unit_config(*proxy_, config->unit_config_id()))) {
    LOG_WARN("remove_unit_config failed", "config_id", config->unit_config_id(), K(ret));
  } else if (OB_FAIL(delete_unit_config(config->unit_config_id(), config->name()))) {
    LOG_WARN("delete_unit_config failed", "config id", config->unit_config_id(),
             "name", config->name(), K(ret));
  } else {
    ROOTSERVICE_EVENT_ADD("unit", "drop_resource_unit",
                          "name", config->name());
    // free memory
    config_allocator_.free(config);
    config = NULL;
  }
  LOG_INFO("finish drop unit config", K(name), K(ret));
  return ret;
}

int ObUnitManager::check_tenant_pools_in_shrinking(
    const uint64_t tenant_id,
    bool &is_shrinking)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObResourcePool *> *pools = NULL;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(NULL == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools ptr is null", K(ret), KP(pools));
  } else {
    is_shrinking = false;
    for (int64_t i = 0; !is_shrinking && OB_SUCC(ret) && i < pools->count(); ++i) {
      common::ObArray<share::ObUnit *> *units = NULL;
      const share::ObResourcePool *pool = pools->at(i);
      if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", K(ret), "pool_id", pool->resource_pool_id_);
      } else if (NULL == units) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptrs is null", K(ret), KP(units));
      } else {
        is_shrinking = false;
        for (int64_t j = 0; !is_shrinking && OB_SUCC(ret) && j < units->count(); ++j) {
          const ObUnit *unit = units->at(j);
          if (OB_UNLIKELY(NULL == unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr si null", K(ret));
          } else if (ObUnit::UNIT_STATUS_DELETING == unit->status_) {
            is_shrinking = true;
          } else if (ObUnit::UNIT_STATUS_ACTIVE == unit->status_) {
            // a normal unit, go on and check next
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit status unexpected", K(ret), "unit_status", unit->status_);
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_pool_in_shrinking(
    const uint64_t pool_id,
    bool &is_shrinking)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(OB_INVALID_ID == pool_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(pool_id));
  } else {
    common::ObArray<share::ObUnit *> *units = NULL;
    SpinRLockGuard guard(lock_);
    if (OB_FAIL(get_units_by_pool(pool_id, units))) {
      LOG_WARN("fail to get units by pool", K(ret), K(pool_id));
    } else if (NULL == units) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("units ptr is null", K(ret), KP(units));
    } else {
      is_shrinking = false;
      for (int64_t i = 0; !is_shrinking && OB_SUCC(ret) && i < units->count(); ++i) {
        const ObUnit *unit = units->at(i);
        if (OB_UNLIKELY(NULL == unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret));
        } else if (ObUnit::UNIT_STATUS_DELETING == unit->status_) {
          is_shrinking = true;
        } else if (ObUnit::UNIT_STATUS_ACTIVE == unit->status_) {
          // a normal unit, go on and check next
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit status unexpected", K(ret), "unit_status", unit->status_);
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::create_resource_pool(
    share::ObResourcePool &resource_pool,
    const ObUnitConfigName &config_name,
    const bool if_not_exist)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (OB_FAIL(inner_create_resource_pool(resource_pool, config_name, if_not_exist))) {
    LOG_WARN("fail to inner create resource pool", K(ret));
  }
  return ret;
}

int ObUnitManager::inner_create_resource_pool(
    share::ObResourcePool &resource_pool,
    const ObUnitConfigName &config_name,
    const bool if_not_exist)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start create resource pool", K(resource_pool), K(config_name));
  ObUnitConfig *config = NULL;
  share::ObResourcePool *pool = NULL;
  const char *module = "CREATE_RESOURCE_POOL";
  bool is_bootstrap_pool = (ObUnitConfig::SYS_UNIT_CONFIG_ID == resource_pool.unit_config_id_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (is_bootstrap_pool && OB_FAIL(check_bootstrap_pool(resource_pool))) {
    LOG_WARN("check bootstrap pool failed", K(resource_pool), K(ret));
  } else if (!is_bootstrap_pool && OB_FAIL(check_resource_pool(resource_pool))) {
    LOG_WARN("check_resource_pool failed", K(resource_pool), K(ret));
  } else if (config_name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource unit name");
    LOG_WARN("invalid config_name", K(config_name), K(ret));
  } else if (OB_FAIL(get_unit_config_by_name(config_name, config))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_unit_config_by_name failed", K(config_name), K(ret));
    } else {
      ret = OB_RESOURCE_UNIT_NOT_EXIST;
      LOG_USER_ERROR(OB_RESOURCE_UNIT_NOT_EXIST, to_cstring(config_name));
      LOG_WARN("config not exist", K(config_name), K(ret));
    }
  } else if (NULL == config) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("config is null", KP(config), K(ret));
  } else if (OB_SUCCESS == (ret = inner_get_resource_pool_by_name(resource_pool.name_, pool))) {
    if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool is null", K_(resource_pool.name), KP(pool), K(ret));
    } else if (if_not_exist) {
      LOG_USER_NOTE(OB_RESOURCE_POOL_EXIST, to_cstring(resource_pool.name_));
      LOG_INFO("resource_pool already exist, no need to create", K(resource_pool.name_));
    } else {
      ret = OB_RESOURCE_POOL_EXIST;
      LOG_USER_ERROR(OB_RESOURCE_POOL_EXIST, to_cstring(resource_pool.name_));
      LOG_WARN("resource_pool already exist", "name", resource_pool.name_, K(ret));
    }
  } else if (OB_ENTRY_NOT_EXIST != ret) {
    LOG_WARN("get resource pool by name failed", "name", resource_pool.name_, K(ret));
  } else {
    ret = OB_SUCCESS;
    common::ObMySQLTransaction trans;
    share::ObResourcePool *new_pool = NULL;
    const int64_t min_full_resource_pool_memory = GCONF.__min_full_resource_pool_memory;
    if (NULL == (new_pool = pool_allocator_.alloc())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc memory failed", K(ret));
    } else if (REPLICA_TYPE_FULL == resource_pool.replica_type_
        && config->memory_size() < min_full_resource_pool_memory) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("full resource pool min memory illegal", KR(ret), K(config->memory_size()),
          K(min_full_resource_pool_memory));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unit MEMORY_SIZE less than __min_full_resource_pool_memory");
    } else {
      if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
        LOG_WARN("start transaction failed", K(ret));
      } else {
        if (OB_FAIL(new_pool->assign(resource_pool))) {
          LOG_WARN("failed to assign new_pool", K(ret));
        } else {
          new_pool->unit_config_id_ = config->unit_config_id();
          if (OB_INVALID_ID == new_pool->resource_pool_id_) {
            if (OB_FAIL(fetch_new_resource_pool_id(new_pool->resource_pool_id_))) {
              LOG_WARN("fetch_new_resource_pool_id failed", K(ret));
            }
          } else {
            // sys resource pool with pool_id set
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, *new_pool))) {
          LOG_WARN("update_resource_pool failed", "resource_pool", *new_pool, K(ret));
        } else if (OB_FAIL(update_pool_map(new_pool))) {
          LOG_WARN("update pool map failed", "resource_pool", *new_pool, K(ret));
        }

        if (OB_SUCCESS == ret && !is_bootstrap_resource_pool(new_pool->resource_pool_id_)) {
          if (OB_FAIL(allocate_new_pool_units_(trans, *new_pool, module))) {
            LOG_WARN("arrange pool units failed", K(module), KR(ret), "resource_pool", *new_pool);
          }
        }
      }

      if (trans.is_started()) {
        const bool commit = (OB_SUCC(ret));
        int temp_ret = OB_SUCCESS;
        if (OB_SUCCESS != (temp_ret = trans.end(commit))) {
          LOG_WARN("trans end failed", K(commit), K(temp_ret));
          ret = (OB_SUCCESS == ret) ? temp_ret : ret;
        }
      }
      if (OB_SUCC(ret)) {
        ret = E(EventTable::EN_UNIT_MANAGER) OB_SUCCESS;
        DEBUG_SYNC(UNIT_MANAGER_WAIT_FOR_TIMEOUT);
      }

      if (OB_FAIL(ret)) {
        if (OB_INVALID_ID == new_pool->resource_pool_id_) {
          // do nothing, fetch new resource pool id failed
        } else {
          int temp_ret = OB_SUCCESS; // avoid ret overwritten
          // some error occur during doing the transaction, rollback change occur in memory
          ObArray<ObUnit *> *units = NULL;
          if (OB_SUCCESS == (temp_ret = get_units_by_pool(new_pool->resource_pool_id_, units))) {
            if (NULL == units) {
              temp_ret = OB_ERR_UNEXPECTED;
              LOG_WARN("units is null", KP(units), K(temp_ret));
            } else if (OB_SUCCESS != (temp_ret = delete_units_of_pool(
                        new_pool->resource_pool_id_))) {
              LOG_WARN("delete_units_of_pool failed", "resource_pool_id",
                       new_pool->resource_pool_id_, K(temp_ret));
            }
          } else if (OB_ENTRY_NOT_EXIST != temp_ret) {
            LOG_WARN("get_units_by_pool failed",
                     "resource_pool_id", new_pool->resource_pool_id_, K(temp_ret));
          } else {
            temp_ret = OB_SUCCESS;
          }

          share::ObResourcePool *temp_pool = NULL;
          if (OB_SUCCESS != temp_ret) {
          } else if (OB_SUCCESS != (temp_ret = get_resource_pool_by_id(
                      new_pool->resource_pool_id_, temp_pool))) {
            if (OB_ENTRY_NOT_EXIST != temp_ret) {
              LOG_WARN("get_resource_pool_by_id failed", "pool_id", new_pool->resource_pool_id_,
                       K(temp_ret));
            } else {
              temp_ret = OB_SUCCESS;
              // do nothing, no need to delete from id_map and name_map
            }
          } else if (NULL == temp_pool) {
            temp_ret = OB_ERR_UNEXPECTED;
            LOG_WARN("temp_pool is null", KP(temp_pool), K(temp_ret));
          } else if (OB_SUCCESS != (temp_ret = delete_resource_pool(
                      new_pool->resource_pool_id_, new_pool->name_))) {
            LOG_WARN("delete_resource_pool failed", "new pool", *new_pool, K(temp_ret));
          }
        }
        // avoid memory leak
        pool_allocator_.free(new_pool);
        new_pool = NULL;
      } else {
        // inc unit config ref count at last
        if (OB_FAIL(inc_config_ref_count(config->unit_config_id()))) {
          LOG_WARN("inc_config_ref_count failed", "config id", config->unit_config_id(), K(ret));
        } else if (OB_FAIL(insert_config_pool(config->unit_config_id(), new_pool))) {
          LOG_WARN("insert config pool failed", "config id", config->unit_config_id(), K(ret));
        } else {
          ROOTSERVICE_EVENT_ADD("unit", "create_resource_pool",
                                "name", new_pool->name_,
                                "unit", config_name,
                                "zone_list", new_pool->zone_list_);
        }
      }
    }
  }
  LOG_INFO("finish create resource pool", K(resource_pool), K(config_name), K(ret));
  return ret;
}

int ObUnitManager::convert_pool_name_list(
    const common::ObIArray<common::ObString> &split_pool_list,
    common::ObIArray<share::ObResourcePoolName> &split_pool_name_list)
{
  int ret = OB_SUCCESS;
  split_pool_name_list.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < split_pool_list.count(); ++i) {
    share::ObResourcePoolName pool_name;
    if (OB_FAIL(pool_name.assign(split_pool_list.at(i).ptr()))) {
      LOG_WARN("fail to assign pool name", K(ret));
    } else if (OB_FAIL(split_pool_name_list.push_back(pool_name))) {
      LOG_WARN("fail to push back", K(ret));
    }
  }
  return ret;
}

int ObUnitManager::check_split_pool_name_condition(
    const common::ObIArray<share::ObResourcePoolName> &split_pool_name_list)
{
  int ret = OB_SUCCESS;
  // Check whether the pool name already exists,
  // and check whether the pool name is duplicate
  const int64_t POOL_NAME_SET_BUCKET_NUM = 16;
  ObHashSet<share::ObResourcePoolName> pool_name_set;
  if (OB_FAIL(pool_name_set.create(POOL_NAME_SET_BUCKET_NUM))) {
    LOG_WARN("fail to create hash set", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < split_pool_name_list.count(); ++i) {
      const share::ObResourcePoolName &pool_name = split_pool_name_list.at(i);
      share::ObResourcePool *pool = NULL;
      int tmp_ret = inner_get_resource_pool_by_name(pool_name, pool);
      if (OB_ENTRY_NOT_EXIST == tmp_ret) {
        // good, go on next, this pool name not exist
      } else if (OB_SUCCESS == tmp_ret) {
        if (NULL == pool) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool is null", K(ret), KP(pool), K(pool_name));
        } else {
          ret = OB_RESOURCE_POOL_EXIST;
          LOG_WARN("resource pool already exist", K(ret), K(pool_name));
          LOG_USER_ERROR(OB_RESOURCE_POOL_EXIST, to_cstring(pool_name));
        }
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error when get pool by name", K(ret), K(pool_name));
      }
      if (OB_SUCC(ret)) {
        int tmp_ret = pool_name_set.exist_refactored(pool_name);
        if (OB_HASH_NOT_EXIST == tmp_ret) {
          if (OB_FAIL(pool_name_set.set_refactored(pool_name))) {
            LOG_WARN("fail to set", K(ret), K(pool_name));
          }
        } else if (OB_HASH_EXIST == tmp_ret) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("invalid argument to split resource pool with duplicated name");
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool name");
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error when set hashset", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_split_pool_zone_condition(
    const common::ObIArray<common::ObZone> &split_zone_list,
    const share::ObResourcePool &pool)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObZone> &pool_zone_list = pool.zone_list_;
  // Check whether the zone is included in the pool zone list,
  // and check whether the zone is duplicated
  const int64_t ZONE_SET_BUCKET_NUM = 16;
  ObHashSet<common::ObZone> zone_set;
  if (pool_zone_list.count() != split_zone_list.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument to split zone list", K(ret));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "corresponding zone vector");
  } else if (OB_FAIL(zone_set.create(ZONE_SET_BUCKET_NUM))) {
    LOG_WARN("fail to create hash set", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < split_zone_list.count(); ++i) {
      const common::ObZone &this_zone = split_zone_list.at(i);
      if (!has_exist_in_array(pool_zone_list, this_zone)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid argument to non-exist zone in splitting zone vector", K(ret));
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "non-exist zone in corresponding zone vector");
      } else {
        int tmp_ret = zone_set.exist_refactored(this_zone);
        if (OB_HASH_NOT_EXIST == tmp_ret) {
          if (OB_FAIL(zone_set.set_refactored(this_zone))) {
            LOG_WARN("fail to set", K(ret), K(this_zone));
          }
        } else if (OB_HASH_EXIST == tmp_ret) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("invalid argument to duplicate zones in splitting zone vector", K(ret));
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "duplidate zones in corresponding zone vector");
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error when set hashset", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::split_resource_pool(
    const share::ObResourcePoolName &pool_name,
    const common::ObIArray<common::ObString> &split_pool_list,
    const common::ObIArray<common::ObZone> &split_zone_list)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start split resource pool", K(pool_name), K(split_pool_list), K(split_zone_list));
  SpinWLockGuard guard(lock_);
  share::ObResourcePool *pool = NULL;
  common::ObArray<share::ObResourcePoolName> split_pool_name_list;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (pool_name.is_empty()
             || split_pool_list.count() <= 0
             || split_zone_list.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("resource pool name is empty", K(ret), K(split_zone_list), K(split_pool_list));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool");
  } else if (OB_FAIL(inner_get_resource_pool_by_name(pool_name, pool))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get resource pool by name failed", K(ret), K(pool_name));
    } else {
      ret = OB_RESOURCE_POOL_NOT_EXIST;
      LOG_WARN("resource pool not exist", K(ret), K(pool_name));
      LOG_USER_ERROR(OB_RESOURCE_POOL_NOT_EXIST, to_cstring(pool_name));
    }
  } else if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool ptr is null", K(ret));
  } else if (OB_FAIL(convert_pool_name_list(split_pool_list, split_pool_name_list))) {
    LOG_WARN("fail to convert pool name list", K(ret));
  } else if (OB_FAIL(check_split_pool_name_condition(split_pool_name_list))) {
    LOG_WARN("fail to check pool list name duplicate", K(ret));
  } else if (split_pool_name_list.count() != split_zone_list.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument to split pool and zone count", K(ret));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "split pool and zone count");
  } else if (OB_FAIL(check_split_pool_zone_condition(split_zone_list, *pool))) {
    LOG_WARN("fail to check split pool zone condition", K(ret));
  } else if (OB_FAIL(do_split_resource_pool(
          pool, split_pool_name_list, split_zone_list))) {
    LOG_WARN("fail to do split resource pool", K(ret));
  } else {
    LOG_INFO("succeed to split resource pool", K(pool_name),
             "new_pool_name", split_pool_list,
             "corresponding_zone", split_zone_list);
  }
  return ret;
}

int ObUnitManager::do_split_pool_persistent_info(
    share::ObResourcePool *pool,
    const common::ObIArray<share::ObResourcePoolName> &split_pool_name_list,
    const common::ObIArray<common::ObZone> &split_zone_list,
    common::ObIArray<share::ObResourcePool *> &allocate_pool_ptrs)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("fail to start transaction", K(ret));
  } else if (OB_UNLIKELY(split_zone_list.count() != split_pool_name_list.count()
                         || NULL == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    // Write down the resource pool allocated during execution
    allocate_pool_ptrs.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < split_pool_name_list.count(); ++i) {
      const share::ObResourcePoolName &new_pool_name = split_pool_name_list.at(i);
      const common::ObZone &zone = split_zone_list.at(i);
      share::ObResourcePool *new_pool = NULL;
      if (NULL == (new_pool = pool_allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("fail to alloc memory", K(ret));
      } else if (OB_FAIL(allocate_pool_ptrs.push_back(new_pool))) {
        LOG_WARN("fail to push back", K(ret));
      } else if (OB_FAIL(fill_splitting_pool_basic_info(new_pool_name, new_pool, zone, pool))) {
        LOG_WARN("fail to fill splitting pool basic info", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < allocate_pool_ptrs.count(); ++i) {
      share::ObResourcePool *new_pool = allocate_pool_ptrs.at(i);
      if (OB_UNLIKELY(NULL == new_pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret));
      } else if (new_pool->zone_list_.count() != 1) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("zone list count unexpected", K(ret), "zone_list", new_pool->zone_list_);
      } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, *new_pool))) {
        LOG_WARN("fail to update resource pool", K(ret));
      } else if (OB_FAIL(split_pool_unit_persistent_info(
              trans, new_pool->zone_list_.at(0), new_pool, pool))) {
        LOG_WARN("fail to split pool unit persistent info", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (ut_operator_.remove_resource_pool(trans, pool->resource_pool_id_)) {
      LOG_WARN("fail to remove resource pool persistent info", K(ret),
               "pool_id", pool->resource_pool_id_);
    } else {} // all persistent infos update finished
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("fail to end trans", K(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_FAIL(ret)) {
      // 1. The transaction did fail to commit.
      //    the internal table and the memory state remain the same,
      //    as long as the memory of allocate_pool_ptrs is released.
      //
      // 2. The transaction submission timed out, but the final submission was successful.
      //    The internal table and memory state are inconsistent,
      //    and the outer layer will call the reload unit manager.
      //    Still need to release the memory of allocate_pool_ptrs
      for (int64_t i = 0; i < allocate_pool_ptrs.count(); ++i) {
        share::ObResourcePool *new_pool = allocate_pool_ptrs.at(i);
        if (NULL != new_pool) {
          pool_allocator_.free(new_pool);
          new_pool = NULL;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::do_split_pool_inmemory_info(
    share::ObResourcePool *pool,
    common::ObIArray<share::ObResourcePool *> &allocate_pool_ptrs)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < allocate_pool_ptrs.count(); ++i) {
      share::ObResourcePool *new_pool = allocate_pool_ptrs.at(i);
      if (OB_UNLIKELY(NULL == new_pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret));
      } else if (new_pool->zone_list_.count() != 1) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("zone list count unexpected", K(ret), "zone_list", new_pool->zone_list_);
      } else if (OB_FAIL(inc_config_ref_count(new_pool->unit_config_id_))) {
        LOG_WARN("fail to inc config ref count", K(ret),
                 "unit_config_id", new_pool->unit_config_id_);
      } else if (OB_FAIL(insert_config_pool(new_pool->unit_config_id_, new_pool))) {
        LOG_WARN("fail to insert config pool", K(ret),
                 "unit_config_id", new_pool->unit_config_id_);
      } else if (OB_FAIL(update_pool_map(new_pool))) {
        LOG_WARN("fail to update pool map", K(ret),
                 "resource_pool_id", new_pool->resource_pool_id_);
      } else if (!new_pool->is_granted_to_tenant()) {
        // bypass
      } else if (OB_FAIL(insert_tenant_pool(new_pool->tenant_id_, new_pool))) {
        LOG_WARN("fail to insert tenant pool", K(ret), "tenant_id", new_pool->tenant_id_);
      }
      if (OB_FAIL(ret)) {
        // failed
      } else if (OB_FAIL(split_pool_unit_inmemory_info(
              new_pool->zone_list_.at(0), new_pool, pool))) {
        LOG_WARN("fail to split pool unit inmemory info", K(ret));
      } else {} // no more
    }
    common::ObArray<share::ObUnit *> *pool_units = NULL;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(dec_config_ref_count(pool->unit_config_id_))) {
      LOG_WARN("fail to dec config ref count", K(ret));
    } else if (OB_FAIL(delete_config_pool(pool->unit_config_id_, pool))) {
      LOG_WARN("fail to delete config pool", K(ret));
    } else if (OB_FAIL(delete_resource_pool(pool->resource_pool_id_, pool->name_))) {
      LOG_WARN("fail to delete resource pool", K(ret));
    } else if (!pool->is_granted_to_tenant()) {
      // bypass
    } else if (OB_FAIL(delete_tenant_pool(pool->tenant_id_, pool))) {
      LOG_WARN("fail to delete tenant pool", K(ret), "tenant_id", pool->tenant_id_);
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, pool_units))) {
      LOG_WARN("fail to get units by pool", K(ret), "pool_id", pool->resource_pool_id_);
    } else if (OB_UNLIKELY(NULL == pool_units)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool units ptr is null", K(ret));
    } else if (OB_FAIL(pool_unit_map_.erase_refactored(pool->resource_pool_id_))) {
      LOG_WARN("fail to erase map", K(ret), "pool_id", pool->resource_pool_id_);
    } else {
      pool_unit_allocator_.free(pool_units);
      pool_units = NULL;
      pool_allocator_.free(pool);
      pool = NULL;
    }
    if (OB_FAIL(ret)) {
      // reload
      rootserver::ObRootService *root_service = NULL;
      if (OB_UNLIKELY(NULL == (root_service = GCTX.root_service_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("rootservice is null", K(ret));
      } else {
        int tmp_ret = root_service->submit_reload_unit_manager_task();
        if (OB_SUCCESS != tmp_ret) {
          LOG_ERROR("fail to reload unit manager", K(tmp_ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::do_split_resource_pool(
    share::ObResourcePool *pool,
    const common::ObIArray<share::ObResourcePoolName> &split_pool_name_list,
    const common::ObIArray<common::ObZone> &split_zone_list)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObResourcePool *> allocate_pool_ptrs;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (split_pool_name_list.count() != split_zone_list.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("split pool name and zone count not match",
             K(ret), K(split_pool_name_list), K(split_zone_list));
  } else if (OB_FAIL(do_split_pool_persistent_info(
          pool, split_pool_name_list, split_zone_list, allocate_pool_ptrs))) {
    LOG_WARN("fail to do split pool persistent info", K(ret));
  } else if (OB_FAIL(do_split_pool_inmemory_info(pool, allocate_pool_ptrs))) {
    LOG_WARN("fail to do split pool inmemory info", K(ret));
  } else {} // no more
  return ret;
}

int ObUnitManager::fill_splitting_pool_basic_info(
    const share::ObResourcePoolName &new_pool_name,
    share::ObResourcePool *new_pool,
    const common::ObZone &zone,
    share::ObResourcePool *orig_pool)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(new_pool_name.is_empty() || NULL == new_pool
                         || zone.is_empty() || NULL == orig_pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(new_pool_name), KP(new_pool), K(zone), K(orig_pool));
  } else {
    new_pool->name_ = new_pool_name;
    new_pool->unit_count_ = orig_pool->unit_count_;
    new_pool->unit_config_id_ = orig_pool->unit_config_id_;
    new_pool->tenant_id_ = orig_pool->tenant_id_;
    new_pool->replica_type_ = orig_pool->replica_type_;
    if (OB_FAIL(new_pool->zone_list_.push_back(zone))) {
      LOG_WARN("fail to push back to zone list", K(ret));
    } else if (OB_FAIL(fetch_new_resource_pool_id(new_pool->resource_pool_id_))) {
      LOG_WARN("fail to fetch new resource pool id", K(ret));
    } else {} // finish fill splitting pool basic info
  }
  return ret;
}

int ObUnitManager::split_pool_unit_inmemory_info(
    const common::ObZone &zone,
    share::ObResourcePool *new_pool,
    share::ObResourcePool *orig_pool)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *new_units = NULL;
  common::ObArray<share::ObUnit *> *orig_units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(NULL == new_pool || NULL == orig_pool || zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(new_pool), K(orig_pool), K(zone));
  } else if (OB_FAIL(get_units_by_pool(orig_pool->resource_pool_id_, orig_units))) {
    LOG_WARN("fail to get units by pool", K(ret));
  } else if (OB_UNLIKELY(NULL == orig_units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units ptr is null", K(ret));
  } else {
    ret = get_units_by_pool(new_pool->resource_pool_id_, new_units);
    if (OB_SUCCESS == ret) {
      // got new units
    } else if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("fail to get units by pool", K(ret));
    } else {
      if (NULL == (new_units = pool_unit_allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("fail to alloc memory", K(ret));
      } else if (OB_FAIL(pool_unit_map_.set_refactored(new_pool->resource_pool_id_, new_units))) {
        LOG_WARN("fail to set refactored", K(ret), "pool_id", new_pool->resource_pool_id_);
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < orig_units->count(); ++i) {
      share::ObUnit *unit = orig_units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret));
      } else if (zone != unit->zone_) {
        // bypass
      } else if (NULL == new_units) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("new units ptr is null", K(ret));
      } else {
        unit->resource_pool_id_ = new_pool->resource_pool_id_;
        if (OB_FAIL(new_units->push_back(unit))) {
          LOG_WARN("fail to push back", K(ret));
        } else if (OB_FAIL(update_unit_load(unit, new_pool))) {
          LOG_WARN("fail to update unit load", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::split_pool_unit_persistent_info(
    common::ObMySQLTransaction &trans,
    const common::ObZone &zone,
    share::ObResourcePool *new_pool,
    share::ObResourcePool *orig_pool)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(NULL == new_pool || NULL == orig_pool || zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(new_pool), KP(orig_pool), K(zone));
  } else if (OB_FAIL(get_units_by_pool(orig_pool->resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units ptr is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      const share::ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret));
      } else if (unit->zone_ != zone) {
        // bypass
      } else {
        share::ObUnit new_unit = *unit;
        new_unit.resource_pool_id_ = new_pool->resource_pool_id_;
        if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
          LOG_WARN("fail to update unit", K(ret), K(new_unit));
        } else {
          ROOTSERVICE_EVENT_ADD("unit", "split_pool",
                                "unit_id", unit->unit_id_,
                                "server", unit->server_,
                                "prev_pool_id", orig_pool->resource_pool_id_,
                                "curr_pool_id", new_pool->resource_pool_id_);
        }
      }
    }
  }
  return ret;
}

/* current_unit_num_per_zone: pool->unit_count_
 * complete_unit_num_per_zone: pool->unit_count_ + unit_num_in_deleting
 * has_unit_num_modification: when some unit in deleting this is true, else false
 */
int ObUnitManager::get_tenant_pools_complete_unit_num_and_status(
    const uint64_t tenant_id,
    const common::ObIArray<share::ObResourcePool *> &pools,
    int64_t &complete_unit_num_per_zone,
    int64_t &current_unit_num_per_zone,
    bool &has_unit_num_modification)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else if (OB_UNLIKELY(pools.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(pools));
  } else {
    complete_unit_num_per_zone = -1;
    current_unit_num_per_zone = -1;
    has_unit_num_modification = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      int64_t my_complete_unit_num_per_zone = 0;
      int64_t my_current_unit_num_per_zone = 0;
      bool my_unit_num_modification = false;
      const share::ObResourcePool *pool = pools.at(i);
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", KR(ret), KP(pool), K(tenant_id));
      } else if (tenant_id != pool->tenant_id_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tenant id not match");
      } else if (OB_FAIL(get_pool_complete_unit_num_and_status(
              pool, my_complete_unit_num_per_zone,
              my_current_unit_num_per_zone, my_unit_num_modification))) {
        LOG_WARN("fail to get pool complete unit num and status", KR(ret));
      } else if (-1 == complete_unit_num_per_zone) {
        complete_unit_num_per_zone = my_complete_unit_num_per_zone;
        current_unit_num_per_zone = my_current_unit_num_per_zone;
        has_unit_num_modification = my_unit_num_modification;
      } else if (complete_unit_num_per_zone != my_complete_unit_num_per_zone
          || current_unit_num_per_zone != my_current_unit_num_per_zone
          || has_unit_num_modification != my_unit_num_modification) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tenant pools unit status unexpected", KR(ret), K(tenant_id));
      }
    }
  }
  return ret;
}

int ObUnitManager::determine_alter_resource_tenant_unit_num_type(
    const uint64_t tenant_id,
    const common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t new_unit_num,
    AlterUnitNumType &alter_unit_num_type)
{
  int ret = OB_SUCCESS;
  int64_t complete_unit_num_per_zone = 0;
  int64_t current_unit_num_per_zone = 0;
  bool has_unit_num_modification = true;

  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || new_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(new_unit_num));
  } else if (OB_FAIL(get_tenant_pools_complete_unit_num_and_status(
          tenant_id, pools, complete_unit_num_per_zone,
          current_unit_num_per_zone, has_unit_num_modification))) {
    LOG_WARN("fail to get tenant pools complete unit num and status", KR(ret), K(tenant_id));
  } else if (OB_UNLIKELY(complete_unit_num_per_zone <= 0 || current_unit_num_per_zone <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected complete unit num", KR(ret),
             K(complete_unit_num_per_zone), K(current_unit_num_per_zone));
  } else if (current_unit_num_per_zone == new_unit_num) {
    // when the new unit num is equal to the current unit num, do nothing and return
    alter_unit_num_type = AUN_NOP;
  } else {
    if (has_unit_num_modification) { // a unit num change is taking place
      if (new_unit_num == complete_unit_num_per_zone) {
        // alter unit num to previous, it is a unit num rollback operation
        alter_unit_num_type = AUN_ROLLBACK_SHRINK;
      } else {
        // alter unit num to a value not equal to the previous, an illegal operation
        alter_unit_num_type = AUN_MAX;
      }
    } else { // no unit num change
      if (new_unit_num > current_unit_num_per_zone) {
        alter_unit_num_type = AUN_EXPAND;
      } else {
        alter_unit_num_type = AUN_SHRINK;
      }
    }
  }
  return ret;
}

int ObUnitManager::register_shrink_tenant_pool_unit_num_rs_job(
    const uint64_t tenant_id,
    const int64_t new_unit_num,
    common::ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const int64_t extra_info_len = common::MAX_ROOTSERVICE_EVENT_EXTRA_INFO_LENGTH;
  char extra_info[common::MAX_ROOTSERVICE_EVENT_EXTRA_INFO_LENGTH] = {0};
  if (OB_SUCCESS != (ret = databuff_printf(extra_info, extra_info_len, pos,
          "new_unit_num: %ld", new_unit_num))) {
    if (OB_SIZE_OVERFLOW == ret) {
      LOG_WARN("format to buff size overflow", K(ret));
    } else {
      LOG_WARN("format new unit num failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    int64_t job_id = RS_JOB_CREATE(SHRINK_RESOURCE_TENANT_UNIT_NUM, trans,
                                   "tenant_id", tenant_id,
                                   "extra_info", extra_info);
    if (job_id < 1) {
      ret = OB_SQL_OPT_ERROR;
      LOG_WARN("insert into all_rootservice_job failed", K(ret));
    }
  }
  return ret ;
}

int ObUnitManager::rollback_shrink_tenant_pool_unit_num_rs_job(
    const uint64_t tenant_id,
    common::ObMySQLTransaction &trans)
{
  ObRsJobInfo job_info;
  int ret = RS_JOB_FIND(job_info, trans,
                        "job_type", "SHRINK_RESOURCE_TENANT_UNIT_NUM",
                        "job_status", "INPROGRESS",
                        "tenant_id", tenant_id);
  if (OB_SUCC(ret) && job_info.job_id_ > 0) {
    if (OB_FAIL(RS_JOB_COMPLETE(job_info.job_id_, -1, trans))) { // Roll back, this shrink failed
      LOG_WARN("update all_rootservice_job failed", K(ret), K(job_info));
    }
  } else {
    LOG_WARN("failed to find rs job", K(ret), "tenant_id", tenant_id);
  }
  return ret;
}

int ObUnitManager::complete_shrink_tenant_pool_unit_num_rs_job(
    const uint64_t tenant_id,
    common::ObMySQLTransaction &trans)
{
  ObRsJobInfo job_info;
  int ret = RS_JOB_FIND(job_info, trans,
                        "job_type", "SHRINK_RESOURCE_TENANT_UNIT_NUM",
                        "job_status", "INPROGRESS",
                        "tenant_id", tenant_id);
  if (OB_SUCC(ret) && job_info.job_id_ > 0) {
    if (OB_FAIL(RS_JOB_COMPLETE(job_info.job_id_, 0, trans))) { // job success
      LOG_WARN("update all_rootservice_job failed", K(ret), K(job_info));
    }
  } else {
    LOG_WARN("failed to find rs job", K(ret), "tenant_id" K(tenant_id));
  }
  return ret ;
}

/* generate new unit group id for pools, we hold the assumption that
 * each pool of pools comes from the same tenant and has the same unit num
 */
int ObUnitManager::generate_new_unit_group_id_array(
    const uint64_t tenant_id,
    const common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t new_unit_num,
    common::ObIArray<uint64_t> &unit_group_id_array)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(pools.count() <= 0 || new_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(pools), K(new_unit_num));
  } else {
    {  // defense check
      int64_t sample_unit_num = -1;
      for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
        const share::ObResourcePool *pool = pools.at(i);
        if (OB_UNLIKELY(nullptr == pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool ptr is null", KR(ret), K(tenant_id));
        } else if (-1 == sample_unit_num) {
          sample_unit_num = pool->unit_count_;
        } else if (sample_unit_num != pool->unit_count_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit num not match", KR(ret), K(tenant_id), KPC(pool), K(sample_unit_num));
        }
      }
    }
    const share::ObResourcePool *pool = pools.at(0);
    int64_t unit_group_id_num = 0;
    if (OB_FAIL(ret)) {
      // failed, bypass
    } else if (OB_UNLIKELY(nullptr == pool)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool ptr is null", KR(ret));
    } else if ((unit_group_id_num = new_unit_num - pool->unit_count_) <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit_group_id_num unexpected", KR(ret), K(unit_group_id_num), K(new_unit_num));
    } else {
      unit_group_id_array.reset();
      for (int64_t i = 0; OB_SUCC(ret) && i < unit_group_id_num; ++i) {
        uint64_t unit_group_id = OB_INVALID_ID;
        if (OB_FAIL(fetch_new_unit_group_id(unit_group_id))) {
          LOG_WARN("fail to fetch new unit group id", KR(ret));
        } else if (OB_FAIL(unit_group_id_array.push_back(unit_group_id))) {
          LOG_WARN("fail to push back", KR(ret));
        }
      }
    }
  }
  return ret;
}

/* when alter resource tenant and the new unit num is greater than the current unit num,
 * this func is invoked.
 */
int ObUnitManager::expand_tenant_pools_unit_num_(
    const uint64_t tenant_id,
    common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t new_unit_num,
    const char *module)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  common::ObArray<uint64_t> new_unit_group_id_array;
  ObArray<ObAddr> new_servers;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || new_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(new_unit_num));
  } else if (OB_FAIL(generate_new_unit_group_id_array(
          tenant_id, pools, new_unit_num, new_unit_group_id_array))) {
    LOG_WARN("fail to generate new unit group id array", KR(ret), K(pools), K(new_unit_num));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("fail to start transaction", KR(ret));
  } else {
    share::ObResourcePool new_pool;
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      const bool new_allocate_pool = false;
      const share::ObResourcePool *pool = pools.at(i);
      new_pool.reset();
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected pool", KR(ret), K(tenant_id));
      } else if (pool->tenant_id_ != tenant_id) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tenant id not match", KR(ret),
                 K(tenant_id), "pool_tenant_id", pool->tenant_id_);
      } else if (OB_FAIL(allocate_pool_units_(
              trans, *pool, pool->zone_list_, &new_unit_group_id_array, new_allocate_pool,
              new_unit_num - pool->unit_count_, module, new_servers))) {
        LOG_WARN("fail to allocate pool units", K(module), KR(ret), K(new_unit_num), KPC(pool));
      } else if (OB_FAIL(new_pool.assign(*pool))) {
        LOG_WARN("fail to assign new pool", KR(ret));
      } else if (FALSE_IT(new_pool.unit_count_ = new_unit_num)) {
        // false it, shall never be here
      } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
        LOG_WARN("fail to update resource pool", KR(ret), K(tenant_id));
      }
    }
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("fail to trans end", KR(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool = pools.at(i);
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected pool", KR(ret), K(tenant_id));
      } else {
        pool->unit_count_ = new_unit_num;
      }
    }
  }
  return ret;
}

int ObUnitManager::get_to_be_deleted_unit_group(
    const uint64_t tenant_id,
    const common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t new_unit_num,
    const common::ObIArray<uint64_t> &deleted_unit_group_id_array,
    common::ObIArray<uint64_t> &to_be_deleted_unit_group)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || pools.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(pools));
  } else {
    common::ObArray<uint64_t> all_unit_group_id_array;
    const share::ObResourcePool *pool = pools.at(0);
    const bool is_active = false;
    int64_t to_be_deleted_num = 0;
    if (OB_UNLIKELY(nullptr == pool)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool ptr is null", KR(ret), KP(pool));
    } else if ((to_be_deleted_num = pool->unit_count_ - new_unit_num) <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit num unexpected", KR(ret), KPC(pool), K(tenant_id), K(new_unit_num));
    } else if (OB_FAIL(inner_get_all_unit_group_id(tenant_id, is_active, all_unit_group_id_array))) {
      LOG_WARN("fail to get all unit group id", KR(ret), K(tenant_id));
    } else if (to_be_deleted_num > all_unit_group_id_array.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit num to match", KR(ret), K(tenant_id), K(new_unit_num),
               "old_unit_num", pool->unit_count_,
               "tenant_unit_group_num", all_unit_group_id_array.count());
    } else if (deleted_unit_group_id_array.count() <= 0) {
      // deleted unit groups not specified by the client, we choose for automatically
      for (int64_t i = 0; OB_SUCC(ret) && i < to_be_deleted_num; ++i) {
        if (OB_FAIL(to_be_deleted_unit_group.push_back(all_unit_group_id_array.at(i)))) {
          LOG_WARN("fail to push back", KR(ret));
        }
      }
    } else if (deleted_unit_group_id_array.count() == to_be_deleted_num) {
      // the deleted unit groups are specified by the client
      for (int64_t i = 0; OB_SUCC(ret) && i < deleted_unit_group_id_array.count(); ++i) {
        const uint64_t unit_group_id = deleted_unit_group_id_array.at(i);
        if (has_exist_in_array(all_unit_group_id_array, unit_group_id)) {
          if (OB_FAIL(to_be_deleted_unit_group.push_back(unit_group_id))) {
            LOG_WARN("fail to push back", KR(ret));
          }
        } else {
          ret = OB_OP_NOT_ALLOW;
          LOG_USER_ERROR(OB_OP_NOT_ALLOW, "delete unit group which is not belong to this tenant");
        }
      }
    } else {
      // deleted unit groups num is not match to the specified num.
      ret = OB_OP_NOT_ALLOW;
      LOG_USER_ERROR(OB_OP_NOT_ALLOW, "to be deleted unit num not match");
    }
  }
  return ret;
}

int ObUnitManager::check_shrink_tenant_pools_allowed(
    const uint64_t tenant_id,
    common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t unit_num,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || pools.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(pools));
  } else {
    is_allowed = true;
    for (int64_t i = 0; is_allowed && OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool = pools.at(i);
      bool my_is_allowed = true;
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", KR(ret), KP(pool));
      } else if (tenant_id != pool->tenant_id_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tenant_id not match", KR(ret), K(tenant_id), "pool_tenant_id", pool->tenant_id_);
      } else if (OB_FAIL(check_shrink_granted_pool_allowed(pool, unit_num, my_is_allowed))) {
        LOG_WARN("fail to check shrink granted pool allowed", KR(ret));
      } else {
        is_allowed = my_is_allowed;
      }
    }
  }
  return ret;
}

int ObUnitManager::shrink_tenant_pools_unit_num(
    const uint64_t tenant_id,
    common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t new_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_group_id_array)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  bool is_allowed = false;
  common::ObArray<uint64_t> to_be_deleted_unit_group;

  if (OB_FAIL(check_shrink_tenant_pools_allowed(
          tenant_id, pools, new_unit_num, is_allowed))) {
    LOG_WARN("fail to check shrink tenant pools allowed", KR(ret), K(tenant_id));
  } else if (!is_allowed) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("cannot shrink tenant pools unit num", KR(ret), K(tenant_id));
  } else if (OB_FAIL(get_to_be_deleted_unit_group(
          tenant_id, pools, new_unit_num,
          delete_unit_group_id_array, to_be_deleted_unit_group))) {
    LOG_WARN("fail to get to be deleted unit group", KR(ret), K(tenant_id));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("fail to start transaction", KR(ret));
  } else {
    if (OB_FAIL(register_shrink_tenant_pool_unit_num_rs_job(
            tenant_id, new_unit_num, trans))) {
      LOG_WARN("fail to register shrink tenant pool unit num rs job", KR(ret), K(tenant_id));
    } else {
      share::ObResourcePool new_pool;
      for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
        share::ObResourcePool *pool = pools.at(i);
        common::ObArray<share::ObUnit *> *units = nullptr;
        new_pool.reset();
        if (OB_UNLIKELY(nullptr == pool || tenant_id != pool->tenant_id_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool info unexpected", KR(ret), K(tenant_id), KPC(pool));
        } else if (OB_FAIL(new_pool.assign(*pool))) {
          LOG_WARN("fail to assign new pool", KR(ret), K(tenant_id), KPC(pool));
        } else if (FALSE_IT(new_pool.unit_count_ = new_unit_num)) {
          // shall never be here
        } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
          LOG_WARN("fail to update resource pool", KR(ret), K(tenant_id), K(new_pool));
        } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
          LOG_WARN("fail to get units by pool", KR(ret), KPC(pool), K(tenant_id));
        } else if (OB_UNLIKELY(nullptr == units)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("units ptr is null", KR(ret), K(tenant_id), KPC(pool));
        } else {
          ObUnit new_unit;
          for (int64_t j = 0; OB_SUCC(ret) && j < units->count(); ++j) {
            new_unit.reset();
            const ObUnit *unit = units->at(j);
            if (OB_UNLIKELY(nullptr == unit)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unit ptr is null", KR(ret));
            } else if (!has_exist_in_array(to_be_deleted_unit_group, unit->unit_group_id_)) {
              // bypass
            } else if (FALSE_IT(new_unit = *unit)) {
              // shall never be here
            } else if (FALSE_IT(new_unit.status_ = ObUnit::UNIT_STATUS_DELETING)) {
              // shall never be here
            } else if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
              LOG_WARN("fail to update unit", KR(ret), K(new_unit));
            }
          }
        }
      }
    }
    // however, we need to end this transaction
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("trans end failed", K(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    // modify in memory pool/unit info
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool = pools.at(i);
      common::ObArray<share::ObUnit *> *units = nullptr;
      if (OB_UNLIKELY(nullptr == pool || tenant_id != pool->tenant_id_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool info unexpected", KR(ret), K(tenant_id), KPC(pool));
      } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", KR(ret), KPC(pool), K(tenant_id));
      } else if (OB_UNLIKELY(nullptr == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptr is null", KR(ret), K(tenant_id), KPC(pool));
      } else {
        pool->unit_count_ = new_unit_num;
        for (int64_t j = 0; OB_SUCC(ret) && j < units->count(); ++j) {
          ObUnit *unit = units->at(j);
          if (OB_UNLIKELY(nullptr == unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", KR(ret));
          } else if (!has_exist_in_array(to_be_deleted_unit_group, unit->unit_group_id_)) {
            // bypass
          } else {
            unit->status_ = ObUnit::UNIT_STATUS_DELETING;
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::rollback_tenant_shrink_pools_unit_num(
    const uint64_t tenant_id,
    common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t new_unit_num)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || new_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(new_unit_num));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("start transaction failed", K(ret));
  } else {
    if (OB_FAIL(rollback_shrink_tenant_pool_unit_num_rs_job(tenant_id, trans))) {
      LOG_WARN("rollback rs_job failed ", K(ret));
    } else {
      share::ObResourcePool new_pool;
      for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
        common::ObArray<share::ObUnit *> *units = nullptr;
        const share::ObResourcePool *pool = pools.at(i);
        new_pool.reset();
        if (OB_UNLIKELY(nullptr == pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool ptr is null", KR(ret), K(tenant_id));
        } else if (OB_FAIL(new_pool.assign(*pool))) {
          LOG_WARN("fail to assign new pool", KR(ret), KPC(pool));
        } else if (FALSE_IT(new_pool.unit_count_ = new_unit_num)) {
          // shall never be here
        } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
          LOG_WARN("fail to update resource pool", KR(ret), K(new_pool));
        } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
          LOG_WARN("fail to get units by pool", KR(ret), "pool_id", pool->resource_pool_id_);
        } else if (OB_UNLIKELY(nullptr == units)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("units is null", KR(ret), K(tenant_id), "pool_id", pool->resource_pool_id_);
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
            const ObUnit *this_unit = units->at(i);
            if (OB_UNLIKELY(NULL == this_unit)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unit ptr is null", K(ret));
            } else if (ObUnit::UNIT_STATUS_ACTIVE == this_unit->status_) {
              // go and process the unit in deleting
            } else if (ObUnit::UNIT_STATUS_DELETING == this_unit->status_) {
              ObUnit new_unit = *this_unit;
              new_unit.status_ = ObUnit::UNIT_STATUS_ACTIVE;
              if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
                LOG_WARN("fail to update unit", K(ret), K(new_unit), "cur_unit", *this_unit);
              }
            } else {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unexpected unit status", K(ret), "unit", *this_unit);
            }
          }
        }
      }
    }
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("fail to trans end", KR(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      common::ObArray<share::ObUnit *> *units = nullptr;
      share::ObResourcePool *pool = pools.at(i);
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", KR(ret), K(tenant_id));
      } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", KR(ret), "pool_id", pool->resource_pool_id_);
      } else if (OB_UNLIKELY(nullptr == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units is null", KR(ret), K(tenant_id), "pool_id", pool->resource_pool_id_);
      } else {
        pool->unit_count_ = new_unit_num;
        for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
          ObUnit *this_unit = units->at(i);
          if (OB_UNLIKELY(NULL == this_unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", K(ret));
          } else if (ObUnit::UNIT_STATUS_ACTIVE == this_unit->status_) {
            // go and process the unit in deleting
          } else if (ObUnit::UNIT_STATUS_DELETING == this_unit->status_) {
            this_unit->status_ = ObUnit::UNIT_STATUS_ACTIVE;
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected unit status", K(ret), "unit", *this_unit);
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::alter_resource_tenant(
    const uint64_t tenant_id,
    const int64_t new_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_group_id_array)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start to alter resource tenant", K(tenant_id));
  SpinWLockGuard guard(lock_);
  // related variables
  common::ObArray<share::ObResourcePool *> *pools = nullptr;
  const char *module = "ALTER_RESOURCE_TENANT";
  AlterUnitNumType alter_unit_num_type = AUN_MAX;

  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("fail to check inner stat", KR(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || new_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(new_unit_num));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    LOG_WARN("fail to get pools by tenant", KR(ret), K(tenant_id));
  } else if (OB_UNLIKELY(nullptr == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools ptr is null", KR(ret), K(tenant_id));
  } else if (OB_FAIL(determine_alter_resource_tenant_unit_num_type(
          tenant_id, *pools, new_unit_num, alter_unit_num_type))) {
    LOG_WARN("fail to do determine alter resource tenant unit num type", KR(ret));
  } else if (AUN_NOP == alter_unit_num_type) {
    if (delete_unit_group_id_array.count() > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "delete unit group without unit num change");
    } else {} // good, nothing to do with unit group
  } else if (AUN_EXPAND == alter_unit_num_type) {
    if (delete_unit_group_id_array.count() > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "expand pool unit num combined with deleting unit");
    } else if (OB_FAIL(expand_tenant_pools_unit_num_(
            tenant_id, *pools, new_unit_num, module))) {
      LOG_WARN("fail to expend pool unit num", K(module), KR(ret), K(new_unit_num), K(tenant_id),
          KPC(pools));
    }
  } else if (AUN_SHRINK == alter_unit_num_type) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "shrink resource pool unit number");
    // In 4.0, we do not support shrink resource pool unit number granted to tenant for now
    //if (OB_FAIL(shrink_tenant_pools_unit_num(
    //        tenant_id, *pools, new_unit_num, delete_unit_group_id_array))) {
    //  LOG_WARN("fail to shrink pool unit num", K(ret), K(new_unit_num));
    //}
  } else if (AUN_ROLLBACK_SHRINK == alter_unit_num_type) {
    if (delete_unit_group_id_array.count() > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "rollback shrink pool unit num combined with deleting unit");
    } else if (OB_FAIL(rollback_tenant_shrink_pools_unit_num(
            tenant_id, *pools, new_unit_num))) {
      LOG_WARN("fail to rollbakc shrink pool unit num", K(ret), K(new_unit_num));
    }
  } else if (AUN_MAX == alter_unit_num_type) {
    ret = OB_OP_NOT_ALLOW;
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "alter unit group num while the previous operation is in progress");
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected alter unit group num type", KR(ret), K(alter_unit_num_type));
  }
  return ret;
}

int ObUnitManager::merge_resource_pool(
    const common::ObIArray<common::ObString> &old_pool_list,
    const common::ObIArray<common::ObString> &new_pool_list)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start merge resource pool", K(old_pool_list), K(new_pool_list));
  SpinWLockGuard guard(lock_);
  common::ObArray<share::ObResourcePoolName> old_pool_name_list;
  share::ObResourcePoolName merge_pool_name;
  common::ObArray<share::ObResourcePool *> old_pool;//Pool to be merged
  common::ObArray<common::ObZone> merge_zone_list;//zone list to be merged
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (new_pool_list.count() <= 0
             || old_pool_list.count() <= 0
             || old_pool_list.count() < 2
             //Preventing only one pool from being merged is meaningless
             || new_pool_list.count() > 1) {
    //Can only be merged into one resource pool
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("resource pool zone list is illeagle", K(old_pool_list), K(new_pool_list));
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool");
  } else if (OB_FAIL(convert_pool_name_list(old_pool_list, old_pool_name_list,
                                            new_pool_list, merge_pool_name))) {//1.parse pool name
    LOG_WARN("fail to convert pool name list", K(ret));
  } else if (OB_FAIL(check_old_pool_name_condition(
                     old_pool_name_list, merge_zone_list, old_pool))) {
    //2. check the pool that in old_pool_list is whether valid
    LOG_WARN("fail to check old pool name condition", K(ret));
  } else if (OB_FAIL(check_merge_pool_name_condition(merge_pool_name))) {
    //3. check the merge_pool_name is whether the new pool
    LOG_WARN("fail to check merge pool name condition", K(ret));
  } else if (OB_FAIL(do_merge_resource_pool(merge_pool_name, merge_zone_list, old_pool))) {
    LOG_WARN("fail to do merge resource pool", K(ret));
  } else {
    LOG_INFO("success to merge resource pool", K(merge_pool_name),
             "old_pool_name", old_pool_list);
  }
  return ret;
}

int ObUnitManager::convert_pool_name_list(
    const common::ObIArray<common::ObString> &old_pool_list,
    common::ObIArray<share::ObResourcePoolName> &old_pool_name_list,
    const common::ObIArray<common::ObString> &new_pool_list,
    share::ObResourcePoolName &merge_pool_name)
{
  int ret = OB_SUCCESS;
  old_pool_name_list.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < old_pool_list.count(); ++i) {
    share::ObResourcePoolName pool_name;
    if (OB_FAIL(pool_name.assign(old_pool_list.at(i).ptr()))) {
      LOG_WARN("fail to assign pool name", K(ret));
    } else if (has_exist_in_array(old_pool_name_list, pool_name)) {//Check for duplication
      ret = OB_OP_NOT_ALLOW;
      LOG_WARN("not allow merge resource pool repeat", K(ret));
      LOG_USER_ERROR(OB_OP_NOT_ALLOW, "merge resource pool repeat");
    } else if (OB_FAIL(old_pool_name_list.push_back(pool_name))) {
      LOG_WARN("fail to push back", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(merge_pool_name.assign(new_pool_list.at(0).ptr()))) {
      LOG_WARN("fail to assign pool name", K(ret));
    }
  }
  return ret;
}

int ObUnitManager::check_merge_pool_name_condition(
    const share::ObResourcePoolName &merge_pool_name)
{
  int ret = OB_SUCCESS;
  // check the pool name is whether exist,
  // and check the pool name is whether duplication.
  share::ObResourcePool *pool = NULL;
  int tmp_ret = inner_get_resource_pool_by_name(merge_pool_name, pool);
  if (OB_ENTRY_NOT_EXIST == tmp_ret) {
    // good, go on next, this pool name not exist
  } else if (OB_SUCCESS == tmp_ret) {
    if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool is null", K(ret), KP(pool), K(merge_pool_name));
    } else {
      ret = OB_RESOURCE_POOL_EXIST;
      LOG_WARN("resource pool already exist", K(ret), K(merge_pool_name));
      LOG_USER_ERROR(OB_RESOURCE_POOL_EXIST, to_cstring(merge_pool_name));
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error when get pool by name", K(ret), K(merge_pool_name));
  }
  return ret;
}

int ObUnitManager::check_old_pool_name_condition(
    common::ObIArray<share::ObResourcePoolName> &old_pool_name_list,
    common::ObIArray<common::ObZone> &merge_zone_list,
    common::ObIArray<share::ObResourcePool*> &old_pool)
{
  int ret = OB_SUCCESS;
  common::ObReplicaType replica_type = REPLICA_TYPE_MAX;
  uint64_t tenant_id = OB_INVALID_ID;
  share::ObUnitConfig *unit_config = NULL;
  int64_t unit_count = 0;
  const int64_t POOL_NAME_SET_BUCKET_NUM = 16;
  ObHashSet<share::ObResourcePoolName> pool_name_set;
  if (OB_FAIL(pool_name_set.create(POOL_NAME_SET_BUCKET_NUM))) {
    LOG_WARN("fail to create hash set", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < old_pool_name_list.count(); ++i) {
      const share::ObResourcePoolName &pool_name = old_pool_name_list.at(i);
      share::ObUnitConfig *this_unit_config = NULL;
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_name, pool))) {
        LOG_WARN("fail to get resource pool by name", K(ret));
      } else if (OB_ISNULL(pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret), K(pool), K(pool_name));
      } else {
        if (0 == i) {
          replica_type = pool->replica_type_;
          tenant_id = pool->tenant_id_;
          unit_count = pool->unit_count_;
          if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, unit_config))) {
            if (OB_ENTRY_NOT_EXIST == ret) {
              ret = OB_RESOURCE_UNIT_NOT_EXIST;
            }
            LOG_WARN("can not find config for unit",
                     "unit_config_id", pool->unit_config_id_,
                      K(ret));
          } else if (NULL == unit_config) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("config is null", K(*unit_config), K(ret));
          } else {} //no more
        } else {
          if (replica_type != pool->replica_type_) {
            // Determine whether the replica_type is the same
            ret = OB_OP_NOT_ALLOW;
            LOG_WARN("not allow pool replica type different",
                      K(ret), K(replica_type), K(pool->replica_type_));
            LOG_USER_ERROR(OB_OP_NOT_ALLOW, "pool replica type different");
          } else if (tenant_id != pool->tenant_id_) {
            // Determine whether the tenant_id is the same
            ret = OB_OP_NOT_ALLOW;
            LOG_WARN("not allow pool tenant id different",
                      K(ret), K(tenant_id), K(pool->tenant_id_));
            LOG_USER_ERROR(OB_OP_NOT_ALLOW, "pool tenant id different");
          } else if (unit_count != pool->unit_count_) {
            // Determine whether the unit_count is the same
            ret = OB_OP_NOT_ALLOW;
            LOG_WARN("not allow pool unit count different",
                      K(ret), K(unit_count), K(pool->unit_count_));
            LOG_USER_ERROR(OB_OP_NOT_ALLOW, "pool unit count different");
          } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, this_unit_config))) {
            if (OB_ENTRY_NOT_EXIST == ret) {
              ret = OB_RESOURCE_UNIT_NOT_EXIST;
            }
            LOG_WARN("can not find config for unit",
                     "unit_config_id", pool->unit_config_id_,
                      K(ret));
          } else if (NULL == this_unit_config) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("config is null", KP(this_unit_config), K(ret));
          } else {//Determine whether the unit config is the same
            if (unit_config->unit_config_id() == this_unit_config->unit_config_id()) {//nothing todo
            } else {
              ret = OB_OP_NOT_ALLOW;
              LOG_WARN("not allow pool unit config different",
                        K(ret), K(*this_unit_config), K(*unit_config));
              LOG_USER_ERROR(OB_OP_NOT_ALLOW, "pool unit config different");
            }
          }
        }
        if (OB_SUCC(ret)) {
          int tmp_ret = pool_name_set.exist_refactored(pool_name);
          if (OB_HASH_NOT_EXIST == tmp_ret) {
            if (OB_FAIL(pool_name_set.set_refactored(pool_name))) {
              LOG_WARN("fail to set", K(ret), K(pool_name));
            }
          } else if (OB_HASH_EXIST == tmp_ret) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid argument to merge resource pool duplicate");
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool name duplicate");
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected error when set hashset", K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          // Judge whether the zone has crossover,
          // it is implicitly judged whether the pool_name is repeated, no need to repeat the judgment
          for (int64_t j = 0; OB_SUCC(ret) && j < pool->zone_list_.count(); ++j) {
            const common::ObZone &this_zone = pool->zone_list_.at(j);
            if (has_exist_in_array(merge_zone_list, this_zone)) {
              ret = OB_OP_NOT_ALLOW;
              LOG_WARN("not allow to merge resource pool with duplicated zone");
              LOG_USER_ERROR(OB_OP_NOT_ALLOW, "resource pool with duplicated zone");
            } else if (OB_FAIL(merge_zone_list.push_back(this_zone))) {
              LOG_WARN("fail to push back", K(ret));
            }
          }
        }
        if (OB_SUCC(ret)) {
          ObArray<ObUnit *> *units = nullptr;
          if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
            LOG_WARN("fail to get units by pool", KR(ret), KPC(pool));
          } else if (OB_UNLIKELY(nullptr == units)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("units ptr is null", KR(ret), KPC(pool));
          } else {
            for (int64_t j = 0; OB_SUCC(ret) && j < units->count(); ++j) {
              ObUnit *unit = units->at(j);
              if (OB_UNLIKELY(nullptr == unit)) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("unit ptr is null", KR(ret));
              } else if (unit->status_ != ObUnit::UNIT_STATUS_ACTIVE) {
                ret = OB_OP_NOT_ALLOW;
                LOG_WARN("merging pools when any pool in shrinking not allowed", KR(ret), KPC(pool));
                LOG_USER_ERROR(OB_OP_NOT_ALLOW, "merging pools when any pool in shrinking");
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(old_pool.push_back(pool))) {
            LOG_WARN("fail to push back", K(ret));
          }
        }
      }
    }
  }
 return ret;
}


int ObUnitManager::do_merge_resource_pool(
    const share::ObResourcePoolName &merge_pool_name,
    const common::ObIArray<common::ObZone> &merge_zone_list,
    common::ObIArray<share::ObResourcePool*> &old_pool)
{
  int ret = OB_SUCCESS;
  share::ObResourcePool *allocate_pool_ptr = nullptr;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_FAIL(do_merge_pool_persistent_info(
                     allocate_pool_ptr, merge_pool_name,
                     merge_zone_list, old_pool))) {
    LOG_WARN("fail to do merge pool persistent info", K(ret));
  } else if (OB_FAIL(do_merge_pool_inmemory_info(allocate_pool_ptr, old_pool))) {
    LOG_WARN("fail to do merge pool inmemory info", K(ret));
  } else {} //no more
  return ret;
}

int ObUnitManager::do_merge_pool_persistent_info(
    share::ObResourcePool *&allocate_pool_ptr,
    const share::ObResourcePoolName &merge_pool_name,
    const common::ObIArray<common::ObZone> &merge_zone_list,
    const common::ObIArray<share::ObResourcePool*> &old_pool)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("fail to start transaction", K(ret));
  } else {
    if (NULL == (allocate_pool_ptr = pool_allocator_.alloc())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("fail to alloc memory", K(ret));
    } else if (OB_FAIL(fill_merging_pool_basic_info(allocate_pool_ptr, merge_pool_name,
                                                    merge_zone_list, old_pool))) {
      // The specifications of the pools to be merged are the same, so select the first pool here
      LOG_WARN("fail to fill merging pool basic info", K(ret));
    } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, *allocate_pool_ptr))) {
      LOG_WARN("fail to update resource pool", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < old_pool.count(); ++i) {
        if (OB_FAIL(merge_pool_unit_persistent_info(
                trans, allocate_pool_ptr, old_pool.at(i)))) {
          LOG_WARN("fail to split pool unit persistent info", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < old_pool.count() && OB_SUCC(ret); ++i) {
        if (OB_FAIL(ut_operator_.remove_resource_pool(trans, old_pool.at(i)->resource_pool_id_))) {
          LOG_WARN("fail to remove resource pool persistent info", K(ret),
                   "pool_id", old_pool.at(i)->resource_pool_id_);
        } else {} // all persistent infos update finished
      }
    }
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("fail to end trans", K(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_FAIL(ret)) {
      // 1. The transaction did fail to commit.
      //    the internal table and the memory state remain the same,
      //    as long as the memory of allocate_pool_ptrs is released.
      //
      // 2. The transaction submission timed out, but the final submission was successful.
      //    The internal table and memory state are inconsistent,
      //    and the outer layer will call the reload unit manager.
      //    Still need to release the memory of allocate_pool_ptrs
      if (NULL != allocate_pool_ptr) {
        pool_allocator_.free(allocate_pool_ptr);
        allocate_pool_ptr = NULL;
      }
    }
  }
  return ret;
}

int ObUnitManager::fill_merging_pool_basic_info(
    share::ObResourcePool *&allocate_pool_ptr,
    const share::ObResourcePoolName &merge_pool_name,
    const common::ObIArray<common::ObZone> &merge_zone_list,
    const common::ObIArray<share::ObResourcePool*> &old_pool)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(old_pool.count() <= 1)) {
    //It doesn't make sense to merge only one pool
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  }
  if (OB_SUCC(ret)) {
    share::ObResourcePool *orig_pool = old_pool.at(0);
    if (OB_UNLIKELY(merge_pool_name.is_empty() || NULL == allocate_pool_ptr
                    || NULL == orig_pool)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret), K(merge_pool_name), KP(allocate_pool_ptr), K(orig_pool));
    } else {
      allocate_pool_ptr->name_ = merge_pool_name;
      allocate_pool_ptr->unit_count_ = orig_pool->unit_count_;
      allocate_pool_ptr->unit_config_id_ = orig_pool->unit_config_id_;
      allocate_pool_ptr->tenant_id_ = orig_pool->tenant_id_;
      allocate_pool_ptr->replica_type_ = orig_pool->replica_type_;
      if (OB_FAIL(fetch_new_resource_pool_id(allocate_pool_ptr->resource_pool_id_))) {
        LOG_WARN("fail to fetch new resource pool id", K(ret));
      } else {
        for (int64_t i = 0; i < merge_zone_list.count() && OB_SUCC(ret); ++i) {
          if (OB_UNLIKELY(merge_zone_list.at(i).is_empty())) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid argument", K(ret), K(merge_zone_list.at(i)));
          } else if (OB_FAIL(allocate_pool_ptr->zone_list_.push_back(merge_zone_list.at(i)))) {
            LOG_WARN("fail to push back to zone list", K(ret));
          }
        }
      } // finish fill splitting pool basic info
    }
  }
  return ret;
}

int ObUnitManager::merge_pool_unit_persistent_info(
    common::ObMySQLTransaction &trans,
    share::ObResourcePool *new_pool,
    share::ObResourcePool *orig_pool)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(NULL == new_pool || NULL == orig_pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(new_pool), KP(orig_pool));
  } else if (OB_FAIL(get_units_by_pool(orig_pool->resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units ptr is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      const share::ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret));
      } else {
        share::ObUnit new_unit = *unit;
        new_unit.resource_pool_id_ = new_pool->resource_pool_id_;
        if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
          LOG_WARN("fail to update unit", K(ret), K(new_unit));
        }
        ROOTSERVICE_EVENT_ADD("unit", "merge_pool",
                              "unit_id", unit->unit_id_,
                              "server", unit->server_,
                              "prev_pool_id", orig_pool->resource_pool_id_,
                              "curr_pool_id", new_pool->resource_pool_id_,K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::do_merge_pool_inmemory_info(
    share::ObResourcePool *new_pool/*allocate_pool_ptr*/,
    common::ObIArray<share::ObResourcePool*> &old_pool)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (NULL == new_pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    if (OB_FAIL(inc_config_ref_count(new_pool->unit_config_id_))) {
      LOG_WARN("fail to inc config ref count", K(ret),
               "unit_config_id", new_pool->unit_config_id_);
    } else if (OB_FAIL(insert_config_pool(new_pool->unit_config_id_, new_pool))) {
      LOG_WARN("fail to insert config pool", K(ret),
               "unit_config_id", new_pool->unit_config_id_);
    } else if (OB_FAIL(update_pool_map(new_pool))) {
      LOG_WARN("fail to update pool map", K(ret),
               "resource_pool_id", new_pool->resource_pool_id_);
    } else if (!new_pool->is_granted_to_tenant()) {
      // bypass
    } else if (OB_FAIL(insert_tenant_pool(new_pool->tenant_id_, new_pool))) {
      LOG_WARN("fail to insert tenant pool", K(ret), "tenant_id", new_pool->tenant_id_);
    }
    if (OB_FAIL(ret)) {
      // failed
    } else if (OB_FAIL(merge_pool_unit_inmemory_info(new_pool, old_pool))) {
      LOG_WARN("fail to split pool unit inmemory info", K(ret));
    } else {} // no more
    for (int64_t i = 0 ; i < old_pool.count() && OB_SUCC(ret); ++i) {
      common::ObArray<share::ObUnit *> *pool_units = NULL;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(dec_config_ref_count(old_pool.at(i)->unit_config_id_))) {
        LOG_WARN("fail to dec config ref count", K(ret));
      } else if (OB_FAIL(delete_config_pool(old_pool.at(i)->unit_config_id_, old_pool.at(i)))) {
        LOG_WARN("fail to delete config pool", K(ret));
      } else if (OB_FAIL(delete_resource_pool(old_pool.at(i)->resource_pool_id_,
                                              old_pool.at(i)->name_))) {
        LOG_WARN("fail to delete resource pool", K(ret));
      } else if (OB_INVALID_ID == old_pool.at(i)->tenant_id_) {
        // bypass
      } else if (OB_FAIL(delete_tenant_pool(old_pool.at(i)->tenant_id_, old_pool.at(i)))) {
        LOG_WARN("fail to delete tenant pool", K(ret), "tenant_id", old_pool.at(i)->tenant_id_);
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(get_units_by_pool(old_pool.at(i)->resource_pool_id_, pool_units))) {
        LOG_WARN("fail to get units by pool", K(ret), "pool_id", old_pool.at(i)->resource_pool_id_);
      } else if (OB_UNLIKELY(NULL == pool_units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool units ptr is null", K(ret));
      } else if (OB_FAIL(pool_unit_map_.erase_refactored(old_pool.at(i)->resource_pool_id_))) {
        LOG_WARN("fail to erase map", K(ret), "pool_id", old_pool.at(i)->resource_pool_id_);
      } else {
        pool_unit_allocator_.free(pool_units);
        pool_units = NULL;
        pool_allocator_.free(old_pool.at(i));
        old_pool.at(i) = NULL;
      }
    }
    if (OB_FAIL(ret)) {
      // reload
      rootserver::ObRootService *root_service = NULL;
      if (OB_UNLIKELY(NULL == (root_service = GCTX.root_service_))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("rootservice is null", K(ret));
      } else {
        int tmp_ret = root_service->submit_reload_unit_manager_task();
        if (OB_SUCCESS != tmp_ret) {
          LOG_ERROR("fail to reload unit manager", K(tmp_ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::merge_pool_unit_inmemory_info(
    share::ObResourcePool *new_pool/*allocate_pool_ptr*/,
    common::ObIArray<share::ObResourcePool*> &old_pool)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *new_units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(NULL == new_pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(new_pool));
  } else {
    ret = get_units_by_pool(new_pool->resource_pool_id_, new_units);
    if (OB_SUCCESS == ret) {
      // got new units
    } else if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("fail to get units by pool", K(ret));
    } else {
      if (NULL == (new_units = pool_unit_allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("fail to alloc memory", K(ret));
      } else if (OB_FAIL(pool_unit_map_.set_refactored(new_pool->resource_pool_id_, new_units))) {
        LOG_WARN("fail to set refactored", K(ret), "pool_id", new_pool->resource_pool_id_);
      }
    }
    for (int64_t i = 0; i < old_pool.count() && OB_SUCC(ret); ++i) {
      common::ObArray<share::ObUnit *> *orig_units = NULL;
      if (OB_ISNULL(old_pool.at(i))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid argument", K(ret), KP(old_pool.at(i)));
      } else if (OB_FAIL(get_units_by_pool(old_pool.at(i)->resource_pool_id_, orig_units))) {
        LOG_WARN("fail to get units by pool", K(ret));
      } else if (OB_UNLIKELY(NULL == orig_units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptr is null", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < orig_units->count(); ++i) {
          share::ObUnit *unit = orig_units->at(i);
          if (OB_UNLIKELY(NULL == unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", K(ret));
          } else if (NULL == new_units) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("new units ptr is null", K(ret));
          } else {
            unit->resource_pool_id_ = new_pool->resource_pool_id_;
            if (OB_FAIL(new_units->push_back(unit))) {
              LOG_WARN("fail to push back", K(ret));
            } else if (OB_FAIL(update_unit_load(unit, new_pool))) {
              LOG_WARN("fail to update unit load", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::alter_resource_pool(const share::ObResourcePool &alter_pool,
                                       const ObUnitConfigName &config_name,
                                       const common::ObIArray<uint64_t> &delete_unit_id_array)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start alter resource pool", K(alter_pool), K(config_name));
  SpinWLockGuard guard(lock_);
  share::ObResourcePool *pool = NULL;
  share::ObResourcePool  pool_bak;
  // don't invoke alter_pool.is_valid() here, alter_pool.unit_count may be 0
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (alter_pool.name_.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool name");
    LOG_WARN("resource pool name is empty", "resource pool name", alter_pool.name_, K(ret));
  } else if (alter_pool.unit_count_ < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource unit num");
    LOG_WARN("invalid resource pool unit num", "unit num", alter_pool.unit_count_, K(ret));
  } else if (OB_FAIL(inner_get_resource_pool_by_name(alter_pool.name_, pool))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get resource pool by name failed", "resource_pool name", alter_pool.name_, K(ret));
    } else {
      ret = OB_RESOURCE_POOL_NOT_EXIST;
      LOG_USER_ERROR(OB_RESOURCE_POOL_NOT_EXIST, to_cstring(alter_pool.name_));
      LOG_WARN("resource pool not exist", "resource pool name", alter_pool.name_, K(ret));
    }
  } else if (NULL == pool) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool is null", KP(pool), K(ret));
  } else if (REPLICA_TYPE_LOGONLY == pool->replica_type_
             && alter_pool.unit_count_ > 1) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("logonly resource pool should only have one unit on one zone", K(ret), K(alter_pool));
  } else {
    int64_t alter_count = 0;
    if (!config_name.is_empty()) {
      ++alter_count;
    }
    if (0 != alter_pool.unit_count_) {
      ++alter_count;
    }
    if (alter_pool.zone_list_.count() > 0) {
      ++alter_count;
    }
    if (alter_count > 1) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "alter unit_num, resource_unit, zone_list in one cmd");
      LOG_WARN("only support alter one item one time", K(alter_pool), K(config_name), K(ret));
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(pool_bak.assign(*pool))) {
      LOG_WARN("failed to assign pool_bak", K(ret));
    }

    //TODO: modiry replica_type not support;
    // alter unit config
    if (OB_FAIL(ret)) {
    } else if (config_name.is_empty()) {
      // config not change
    } else if (OB_FAIL(alter_pool_unit_config(pool, config_name))) {
      LOG_WARN("alter_pool_unit_config failed", "pool", *pool, K(config_name), K(ret));
    }

    // alter unit num
    if (OB_FAIL(ret)) {
    } else if (0 == alter_pool.unit_count_) {
      // unit num not change
    } else if (OB_FAIL(alter_pool_unit_num(pool, alter_pool.unit_count_, delete_unit_id_array))) {
      LOG_WARN("alter_pool_unit_num failed", "pool", *pool,
               "unit_num", alter_pool.unit_count_, K(ret));
    }

    // alter zone list
    if (OB_FAIL(ret)) {
    } else if (alter_pool.zone_list_.count() <=0) {
      // zone list not change
    } else if (OB_FAIL(alter_pool_zone_list(pool, alter_pool.zone_list_))) {
      LOG_WARN("alter_pool_zone_list failed", "pool", *pool,
               "zone_list", alter_pool.zone_list_, K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    ROOTSERVICE_EVENT_ADD("unit", "alter_resource_pool",
                          "name", pool_bak.name_);
  }
  LOG_INFO("finish alter resource pool", K(alter_pool), K(config_name), K(ret));
  return ret;
}

int ObUnitManager::drop_resource_pool(const ObResourcePoolName &name, const bool if_exist)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start drop resource pool", K(name));
  SpinWLockGuard guard(lock_);
  share::ObResourcePool *pool = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool name");
    LOG_WARN("invalid argument", K(name), K(ret));
  } else if (OB_FAIL(inner_get_resource_pool_by_name(name, pool))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get resource pool by name failed", K(name), K(ret));
    } else {
      if (if_exist) {
        ret = OB_SUCCESS;
        LOG_USER_NOTE(OB_RESOURCE_POOL_NOT_EXIST, to_cstring(name));
        LOG_INFO("resource_pool not exist, but no need drop it", K(name));
      } else {
        ret = OB_RESOURCE_POOL_NOT_EXIST;
        LOG_USER_ERROR(OB_RESOURCE_POOL_NOT_EXIST, to_cstring(name));
        LOG_WARN("resource_pool not exist", K(name), K(ret));
      }
    }
  } else if (NULL == pool) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool is null", KP(pool), K(ret));
  } else if (pool->is_granted_to_tenant()) {
    ret = OB_RESOURCE_POOL_ALREADY_GRANTED;
    LOG_USER_ERROR(OB_RESOURCE_POOL_ALREADY_GRANTED, to_cstring(name));
    LOG_WARN("resource pool is granted to tenant, can't not delete it",
             "tenant_id", pool->tenant_id_, K(ret));
  } else {
    common::ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
      LOG_WARN("start transaction failed", K(ret));
    } else if (OB_FAIL(remove_resource_pool_unit_in_trans(pool->resource_pool_id_,
                                                          trans))) {
      LOG_WARN("failed to remove reource pool and unit", K(ret), K(pool));
    }
    if (trans.is_started()) {
      const bool commit = (OB_SUCC(ret));
      int temp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (temp_ret = trans.end(commit))) {
        LOG_WARN("trans end failed", K(commit), K(temp_ret));
        ret = (OB_SUCCESS == ret) ? temp_ret : ret;
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(delete_resource_pool_unit(pool))) {
      LOG_WARN("failed to delete resource pool or unit", K(ret), K(pool));
    }
  }
  LOG_INFO("finish drop resource pool", K(name), K(ret));
  return ret;
}
int ObUnitManager::remove_resource_pool_unit_in_trans(const int64_t resource_pool_id,
                                                     ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  int migrate_unit_ret = OB_CANCELED;
  if (OB_INVALID_ID == resource_pool_id
      || !trans.is_started()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(resource_pool_id),
             "is_started", trans.is_started());
  } else if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_FAIL(ut_operator_.remove_units(trans, resource_pool_id))) {
    LOG_WARN("remove_units failed", K(ret), K(resource_pool_id));
  } else if (OB_FAIL(ut_operator_.remove_resource_pool(trans, resource_pool_id))) {
    LOG_WARN("remove_resource_pool failed", K(ret), K(resource_pool_id));
  } else if (OB_FAIL(complete_migrate_unit_rs_job_in_pool(resource_pool_id,
                                                          migrate_unit_ret,
                                                          trans))) {
    LOG_WARN("failed to complete migrate unit in pool", K(ret), K(resource_pool_id));
  }
  return ret;
}
// site lock at the call
int ObUnitManager::delete_resource_pool_unit(share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is null or invalid", K(ret), K(pool));
  } else if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else {
    if (OB_FAIL(delete_units_of_pool(pool->resource_pool_id_))) {
      LOG_WARN("delete_units_of_pool failed", "pool id", pool->resource_pool_id_, K(ret));
    } else if (OB_FAIL(delete_resource_pool(pool->resource_pool_id_, pool->name_))) {
      LOG_WARN("delete_resource_pool failed", "pool", *pool, K(ret));
    } else {
      const uint64_t config_id = pool->unit_config_id_;
      if (OB_FAIL(dec_config_ref_count(config_id))) {
        LOG_WARN("dec_config_ref_count failed", K(config_id), K(ret));
      } else if (OB_FAIL(delete_config_pool(config_id, pool))) {
        LOG_WARN("delete config pool failed", K(config_id), "pool", *pool, K(ret));
      } else {
        ROOTSERVICE_EVENT_ADD("unit", "drop_resource_pool",
                              "name", pool->name_);
      }
      pool_allocator_.free(pool);
      pool = NULL;
    }
  }
  return ret;
}
int ObUnitManager::drop_standby_resource_pool(const common::ObIArray<ObResourcePoolName> &pool_names,
                                              ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  if (0 == pool_names.count() || !trans.is_started()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool names is empty or trans not start", K(ret),
        K(pool_names), "started", trans.is_started());
  } else if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else {
    SpinRLockGuard guard(lock_);
    //When the standby database deletes a tenant, it needs to delete resource_pool
    for (int64_t i = 0; i < pool_names.count() && OB_SUCC(ret); ++i) {
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        LOG_WARN("failed to get reource pool by name", K(ret), K(i), K(pool_names));
      } else if (OB_ISNULL(pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret), K(pool));
      } else if (OB_FAIL(remove_resource_pool_unit_in_trans(
              pool->resource_pool_id_, trans))) {
        LOG_WARN("failed to remove resource pool and unit", K(ret), K(pool));
      }
    }
  }
  return ret;
}
int ObUnitManager::commit_drop_standby_resource_pool(const common::ObIArray<ObResourcePoolName> &pool_names)
{
  int ret = OB_SUCCESS;
  if (0 == pool_names.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool names is empty or trans not start", K(ret), K(pool_names));
  } else if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else {
    SpinWLockGuard guard(lock_);
    //When the standby database deletes a tenant, it needs to delete resource_pool
    for (int64_t i = 0; i < pool_names.count() && OB_SUCC(ret); ++i) {
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        LOG_WARN("failed to get reource pool by name", K(ret), K(i), K(pool_names));
      } else if (OB_ISNULL(pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret), K(pool));
      } else if (OB_FAIL(delete_resource_pool_unit(pool))) {
        LOG_WARN("failed to remove resource pool and unit", K(ret), K(pool));
      }
    }
  }
  return ret;
}
//After the 14x version,
//the same tenant is allowed to have multiple unit specifications in a zone,
//but it is necessary to ensure that these units can be scattered on each server in the zone,
//and multiple units of the same tenant cannot be located on the same machine;
int ObUnitManager::check_server_enough(const uint64_t tenant_id,
                                       const ObIArray<ObResourcePoolName> &pool_names,
                                       bool &enough)
{
  int ret = OB_SUCCESS;
  enough = true;
  share::ObResourcePool *pool = NULL;
  ObArray<ObUnitInfo> unit_infos;
  ObArray<ObUnitInfo> total_unit_infos;
  common::ObArray<share::ObResourcePool *> *pools = NULL;;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pool_names.count() <= 0 || !is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_names), K(tenant_id), K(ret));
  } else {
    //Count the number of newly added units
    for (int64_t i = 0; i < pool_names.count() && OB_SUCC(ret); i++) {
      unit_infos.reset();
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        LOG_WARN("fail to get resource pool by name", K(ret), "pool_name", pool_names.at(i));
      } else if (OB_ISNULL(pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get invalid pool", K(ret), K(pool));
      } else if (OB_FAIL(inner_get_unit_infos_of_pool(pool->resource_pool_id_, unit_infos))) {
        LOG_WARN("fail to get unit infos", K(ret), K(*pool));
      } else {
        for (int64_t j = 0; j < unit_infos.count() && OB_SUCC(ret); j++) {
          if (OB_FAIL(total_unit_infos.push_back(unit_infos.at(j)))) {
            LOG_WARN("fail to push back unit", K(ret), K(total_unit_infos), K(j), K(unit_infos));
          } else {
            LOG_DEBUG("add unit infos", K(ret), K(total_unit_infos), K(unit_infos));
          }
        }
      } //end else
    } // end for
  }
  //Count the number of existing units
  if (OB_FAIL(ret)) {
    //nothing todo
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      // a new tenant, without resource pool already granted
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
    }
  } else if (OB_UNLIKELY(NULL == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools is null", K(ret), KP(pools));
  } else {
    for (int64_t i = 0; i < pools->count() && OB_SUCC(ret); i++) {
      unit_infos.reset();
      const share::ObResourcePool *pool = pools->at(i);
      if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (OB_FAIL(inner_get_unit_infos_of_pool(pool->resource_pool_id_, unit_infos))) {
        LOG_WARN("fail to get unit infos", K(ret), K(*pool));
      } else {
        for (int64_t j = 0; j < unit_infos.count() && OB_SUCC(ret); j++) {
          if (OB_FAIL(total_unit_infos.push_back(unit_infos.at(j)))) {
            LOG_WARN("fail to push back unit", K(ret), K(total_unit_infos), K(j), K(unit_infos));
          } else {
            LOG_WARN("add unit infos", K(ret), K(total_unit_infos), K(unit_infos));
          }
        }
      }
    }
  }
  ObArray<ObZoneInfo> zone_infos;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(zone_mgr_.get_zone(zone_infos))) {
    LOG_WARN("fail to get zone infos", K(ret));
  } else {
    //Count the number of units in zone
    for (int64_t i = 0; i < zone_infos.count() && OB_SUCC(ret) && enough; i++) {
      ObZone zone = zone_infos.at(i).zone_;
      int64_t unit_count = 0;
      int64_t alive_server_count = 0;
      for (int64_t j = 0; j < total_unit_infos.count() && OB_SUCC(ret); j++) {
        if (total_unit_infos.at(j).unit_.zone_ == zone) {
          unit_count ++;
        }
      }
      if (unit_count > 0) {
        if (OB_FAIL(server_mgr_.get_alive_server_count(zone, alive_server_count))) {
          LOG_WARN("fail to get alive server count", K(ret), K(zone));
        } else if (alive_server_count < unit_count) {
          //ret = OB_UNIT_NUM_OVER_SERVER_COUNT;
          enough = false;
          LOG_WARN("resource pool unit num over zone server count", K(ret), K(unit_count), K(alive_server_count),
                   K(total_unit_infos));
        }
      }
    }
  }
  return ret;
}

//The F/L scheme has new restrictions.
//If the logonly replica exists in the locality before adding the Logonly pool, the change is not allowed
int ObUnitManager::check_locality_for_logonly_unit(const share::schema::ObTenantSchema &tenant_schema,
                                                   const ObIArray<ObResourcePoolName> &pool_names,
                                                   bool &is_permitted)
{
  int ret = OB_SUCCESS;
  is_permitted = true;
  ObArray<ObZone> zone_with_logonly_unit;
  ObArray<share::ObZoneReplicaNumSet> zone_locality;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pool_names.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_names), K(ret));
  } else {
    FOREACH_CNT_X(pool_name, pool_names, OB_SUCCESS == ret) {
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(*pool_name, pool))) {
        LOG_WARN("get resource pool by name failed", "pool_name", *pool_name, K(ret));
      } else if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", KP(pool), K(ret));
      } else if (REPLICA_TYPE_LOGONLY != pool->replica_type_) {
        //nothing todo
      } else {
        for (int64_t i = 0; i < pool->zone_list_.count() && OB_SUCC(ret); i++) {
          if (OB_FAIL(zone_with_logonly_unit.push_back(pool->zone_list_.at(i)))) {
            LOG_WARN("fail to push back", K(ret));
          }
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(tenant_schema.get_zone_replica_attr_array(zone_locality))) {
    LOG_WARN("fail to get zone replica attr array", K(ret));
  } else {
    for (int64_t i = 0; i < zone_locality.count() && OB_SUCC(ret); i++) {
      if ((zone_locality.at(i).replica_attr_set_.get_logonly_replica_num() == 1
           || zone_locality.at(i).replica_attr_set_.get_encryption_logonly_replica_num() == 1)
          && has_exist_in_array(zone_with_logonly_unit, zone_locality.at(i).zone_)) {
        is_permitted = false;
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("logonly replica already exist before logonly pool create", K(ret), K(zone_locality),
                 K(zone_with_logonly_unit));
      }
    }
  }
  return ret;
}

int ObUnitManager::grant_pools_for_standby(common::ObISQLClient &client,
                                           const common::ObIArray<share::ObResourcePoolName> &pool_names,
                                           const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
  } else if (common::STANDBY_CLUSTER != ObClusterInfoGetter::get_cluster_role_v2()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not standby cluster", K(ret), "cluster_role", ObClusterInfoGetter::get_cluster_role_v2());
  } else {
    SpinWLockGuard guard(lock_);
    //If the grant pool has been successful, you only need to modify the table next time
    share::ObResourcePool new_pool;
    for (int64_t i = 0; OB_SUCC(ret) && i < pool_names.count(); ++i) {
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        LOG_WARN("get resource pool by name failed", "pool_name", pool_names.at(i), K(ret));
      } else if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", KP(pool), K(ret));
      } else if (pool->is_granted_to_tenant()) {
        ret = OB_RESOURCE_POOL_ALREADY_GRANTED;
        LOG_USER_ERROR(OB_RESOURCE_POOL_ALREADY_GRANTED, to_cstring(pool_names.at(i)));
        LOG_WARN("pool has already granted to other tenant, can't grant again",
            K(ret), K(tenant_id), "pool", *pool);
      } else if (OB_FAIL(new_pool.assign(*pool))) {
        LOG_WARN("failed to assign new_pool", K(ret));
      } else {
        new_pool.tenant_id_ = tenant_id;
        if (OB_FAIL(ut_operator_.update_resource_pool(client, new_pool))) {
          LOG_WARN("update_resource_pool failed", K(new_pool), K(ret));
        }
      }
    }
  }
  return ret;
}

/* when expand zone resource for tenant this func is invoked,
 * we need to check whether the tenant units are in deleting.
 * if any tenant unit is in deleting,
 * @is_allowed returns false
 */
int ObUnitManager::check_expand_zone_resource_allowed_by_old_unit_stat(
    const uint64_t tenant_id,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *cur_pool_array = nullptr;
  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else {
    int tmp_ret = get_pools_by_tenant(tenant_id, cur_pool_array);
    if (OB_ENTRY_NOT_EXIST == tmp_ret) {
      is_allowed = true;
    } else if (OB_UNLIKELY(nullptr == cur_pool_array)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur_pool is null", KR(ret), K(tenant_id));
    } else {
      is_allowed = true;
      for (int64_t i = 0; is_allowed && OB_SUCC(ret) && i < cur_pool_array->count(); ++i) {
        share::ObResourcePool *cur_pool = cur_pool_array->at(i);
        ObArray<share::ObUnit *> *units = nullptr;
        if (OB_UNLIKELY(nullptr == cur_pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("cur pool is null", KR(ret));
        } else if (OB_FAIL(get_units_by_pool(cur_pool->resource_pool_id_, units))) {
          LOG_WARN("fail to get units by pool", KR(ret),
                   "pool_id", cur_pool->resource_pool_id_);
        } else if (OB_UNLIKELY(nullptr == units)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
        } else {
          for (int64_t j = 0; is_allowed && OB_SUCC(ret) && j < units->count(); ++j) {
            if (OB_UNLIKELY(nullptr == units->at(j))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
            } else {
              is_allowed = ObUnit::UNIT_STATUS_ACTIVE == units->at(j)->status_;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_expand_zone_resource_allowed_by_new_unit_stat(
    const common::ObIArray<share::ObResourcePoolName> &pool_names,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  is_allowed = true;
  for (int64_t i = 0; is_allowed && OB_SUCC(ret) && i < pool_names.count(); ++i) {
    share::ObResourcePool *pool = NULL;
    ObArray<ObUnit *> *units = nullptr;
    if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
      LOG_WARN("get resource pool by name failed", "pool_name", pool_names.at(i), K(ret));
    } else if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool is null", KP(pool), K(ret));
    } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
      LOG_WARN("fail to get units by pool", K(ret));
    } else if (OB_UNLIKELY(nullptr == units)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("units ptr is null", K(ret));
    } else {
      for (int64_t j = 0; is_allowed && OB_SUCC(ret) && j < units->count(); ++j) {
        ObUnit *unit = units->at(j);
        if (OB_UNLIKELY(nullptr == unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", KR(ret), KPC(pool));
        } else {
          is_allowed = (unit->status_ == ObUnit::UNIT_STATUS_ACTIVE);
        }
      }
    }
  }
  return ret;
}

/* 1 when this is a tenant being created:
 *   check the input pools, each input pool unit num shall be equal, otherwise illegal
 * 2 when this is a tenant which exists:
 *   check the input pools, each input pool unit num shall be equal to the pools already granted to the tenant.
 */
int ObUnitManager::check_tenant_pools_unit_num_legal(
    const uint64_t tenant_id,
    const common::ObIArray<share::ObResourcePoolName> &input_pool_names,
    bool &unit_num_legal,
    int64_t &sample_unit_num)
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *cur_pool_array = nullptr;
  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("fail to check inner stat", KR(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id)
                        || input_pool_names.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else {
    unit_num_legal = true;
    sample_unit_num = -1;
    int tmp_ret = get_pools_by_tenant(tenant_id, cur_pool_array);
    if (OB_ENTRY_NOT_EXIST == tmp_ret) {
      // when create tenant pools belong to this tenant is empty
    } else if (OB_UNLIKELY(nullptr == cur_pool_array)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur_pool is null", KR(ret), K(tenant_id));
    } else if (OB_UNLIKELY(cur_pool_array->count() <= 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool_array", KR(ret));
    } else if (OB_UNLIKELY(nullptr == cur_pool_array->at(0))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool ptr is null", KR(ret));
    } else {
      sample_unit_num = cur_pool_array->at(0)->unit_count_;
    }
    for (int64_t i = 0; OB_SUCC(ret) && unit_num_legal && i < input_pool_names.count(); ++i) {
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(input_pool_names.at(i), pool))) {
        LOG_WARN("fail to get pool by name", KR(ret), "pool_name", input_pool_names.at(i));
      } else if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", KR(ret), "pool_name", input_pool_names.at(i));
      } else if (-1 == sample_unit_num) {
        sample_unit_num = pool->unit_count_;
      } else if (sample_unit_num == pool->unit_count_) {
        // this is good, unit num matched
      } else {
        unit_num_legal = false;
      }
    }
  }
  return ret;
}

int ObUnitManager::get_pool_unit_group_id_(
    const share::ObResourcePool &pool,
    common::ObIArray<uint64_t> &new_unit_group_id_array)
{
  int ret = OB_SUCCESS;
  if (!pool.is_granted_to_tenant()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < pool.unit_count_; ++i) {
      if (OB_FAIL(new_unit_group_id_array.push_back(0/*not granted to tenant*/))) {
        LOG_WARN("fail to push back", KR(ret));
      }
    }
  } else {
    const bool is_active = false;
    if (OB_FAIL(inner_get_all_unit_group_id(pool.tenant_id_, is_active, new_unit_group_id_array))) {
      LOG_WARN("fail to get all unit group id", KR(ret), K(pool));
    }
  }
  return ret;
}

/* get unit group id for a tenant
 * 1 when bootstrap:
 *   generate unit group id for sys unit group, assign 0 directly
 * 2 when revoke pools for tenant:
 *   after revokes pools, unit group id shall be set to 0 which representing these units
 *   is not belong to any unit group
 * 3 when grant pools for tenant:
 *   3.1 when this is a tenant being created: fetch unit group id from inner table
 *   3.2 when this is a tenant which exists: get unit group id from units granted to this tenant
 */
int ObUnitManager::get_tenant_pool_unit_group_id(
    const bool is_bootstrap,
    const bool grant,
    const uint64_t tenant_id,
    const int64_t unit_group_num,
    common::ObIArray<uint64_t> &new_unit_group_id_array)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || unit_group_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(unit_group_num));
  } else if (is_bootstrap) {
    if (OB_FAIL(new_unit_group_id_array.push_back(OB_SYS_UNIT_GROUP_ID))) {
      LOG_WARN("fail to push back", KR(ret));
    }
  } else {
    if (!grant) {
      // when revoke pools, unit_group_id in units shall be modified to 0
      for (int64_t i = 0; OB_SUCC(ret) && i < unit_group_num; ++i) {
        if (OB_FAIL(new_unit_group_id_array.push_back(
                0/* 0 means this unit doesn't belong to any unit group*/))) {
          LOG_WARN("fail to push back", KR(ret));
        }
      }
    } else {
      // when grant pools, an unit group id greater than 0 is needed for every unit
      ObArray<share::ObResourcePool *> *tenant_pool_array = nullptr;
      int tmp_ret = get_pools_by_tenant(tenant_id, tenant_pool_array);
      if (OB_ENTRY_NOT_EXIST == tmp_ret) {
        // need to fetch unit group from inner table, since this is invoked by create tenant
        for (int64_t i = 0; OB_SUCC(ret) && i < unit_group_num; ++i) {
          uint64_t unit_group_id = OB_INVALID_ID;
          if (OB_FAIL(fetch_new_unit_group_id(unit_group_id))) {
            LOG_WARN("fail to fetch new unit group id", KR(ret));
          } else if (OB_FAIL(new_unit_group_id_array.push_back(unit_group_id))) {
            LOG_WARN("fail to push back", KR(ret));
          }
        }
      } else {
        const bool is_active = false;
        if (OB_FAIL(inner_get_all_unit_group_id(tenant_id, is_active, new_unit_group_id_array))) {
          LOG_WARN("fail to get all unit group id array", KR(ret), K(tenant_id));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_get_all_unit_group_id(
    const uint64_t tenant_id,
    const bool is_active,
    common::ObIArray<uint64_t> &unit_group_array)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else {
    ObArray<share::ObResourcePool *> *cur_pool_array = nullptr;
    ret = get_pools_by_tenant(tenant_id, cur_pool_array);
    if (OB_ENTRY_NOT_EXIST == ret) {
      // bypass
    } else if (OB_SUCCESS != ret) {
      LOG_WARN("fail to get unit group", KR(ret), K(tenant_id));
    } else if (OB_UNLIKELY(nullptr == cur_pool_array)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur pool array ptr is null", KR(ret), K(tenant_id));
    } else if (cur_pool_array->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur pool array ptr is null", KR(ret), K(tenant_id));
    } else {
      share::ObResourcePool *cur_pool = cur_pool_array->at(0);
      ObArray<share::ObUnit *> *units = nullptr;
      common::ObArray<ObUnit *> zone_sorted_unit_array;
      if (OB_UNLIKELY(nullptr == cur_pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("cur pool is null", KR(ret));
      } else if (OB_FAIL(get_units_by_pool(cur_pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", KR(ret),
                 "pool_id", cur_pool->resource_pool_id_);
      } else if (OB_UNLIKELY(nullptr == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
      } else if (OB_FAIL(zone_sorted_unit_array.assign(*units))) {
        LOG_WARN("fail to assign", KR(ret));
      } else if (OB_UNLIKELY(zone_sorted_unit_array.count() < cur_pool->unit_count_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool unit count unexpected", KR(ret), KPC(cur_pool), K(zone_sorted_unit_array));
      } else {
        UnitZoneOrderCmp cmp_operator;
        std::sort(zone_sorted_unit_array.begin(), zone_sorted_unit_array.end(), cmp_operator);
        for (int64_t j = 0; OB_SUCC(ret) && j < zone_sorted_unit_array.count(); ++j) {
          ObUnit *unit = zone_sorted_unit_array.at(j);
          if (OB_ISNULL(unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
          } else if (has_exist_in_array(unit_group_array, unit->unit_group_id_)) {
            //unit_count_ is small than unit group count while some unit is in deleting
            break;
          } else if (is_active && ObUnit::UNIT_STATUS_ACTIVE != unit->status_) {
            //need active unit group
            continue;
          } else if (OB_FAIL(unit_group_array.push_back(unit->unit_group_id_))) {
            LOG_WARN("fail to push back", KR(ret), K(unit));
          }
        }
        if (OB_SUCC(ret) && OB_UNLIKELY(unit_group_array.count() < cur_pool->unit_count_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool unit count unexpected", KR(ret), KPC(cur_pool),
                   K(unit_group_array));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_all_unit_group_id(
    const uint64_t tenant_id,
    common::ObIArray<uint64_t> &unit_group_id_array)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  const bool is_active = false;
  if (OB_FAIL(inner_get_all_unit_group_id(tenant_id, is_active, unit_group_id_array))) {
    LOG_WARN("fail to inner get all unit group id", KR(ret), K(tenant_id));
  }
  return ret;
}

int ObUnitManager::get_all_active_unit_group_id(
    const uint64_t tenant_id,
    common::ObIArray<uint64_t> &active_unit_group_id_array)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  const bool is_active = true;
  if (OB_FAIL(inner_get_all_unit_group_id(tenant_id, is_active, active_unit_group_id_array))) {
    LOG_WARN("fail to inner get all active unit group id", KR(ret), K(tenant_id));
  }
  return ret;
}

int ObUnitManager::check_unit_group_active(
    const uint64_t tenant_id,
    const uint64_t unit_group_id,
    bool &is_active)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else {
    ObArray<share::ObResourcePool *> *cur_pool_array = nullptr;
    ret = get_pools_by_tenant(tenant_id, cur_pool_array);
    if (OB_ENTRY_NOT_EXIST == ret) {
      // bypass
    } else if (OB_SUCCESS != ret) {
      LOG_WARN("fail to get unit group", KR(ret), K(tenant_id));
    } else if (OB_UNLIKELY(nullptr == cur_pool_array)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur pool array ptr is null", KR(ret), K(tenant_id));
    } else if (cur_pool_array->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur pool array ptr is null", KR(ret), K(tenant_id));
    } else {
      share::ObResourcePool *cur_pool = cur_pool_array->at(0);
      ObArray<share::ObUnit *> *units = nullptr;
      if (OB_UNLIKELY(nullptr == cur_pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("cur pool is null", KR(ret));
      } else if (OB_FAIL(get_units_by_pool(cur_pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", KR(ret),
                 "pool_id", cur_pool->resource_pool_id_);
      } else if (OB_UNLIKELY(nullptr == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
      } else {
        bool found = false;
        for (int64_t j = 0; !found && OB_SUCC(ret) && j < units->count(); ++j) {
          if (OB_UNLIKELY(nullptr == units->at(j))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
          } else if (units->at(j)->unit_group_id_ != unit_group_id) {
            // bypass
          } else {
            is_active = units->at(j)->status_ == ObUnit::UNIT_STATUS_ACTIVE;
            found = true;
          }
        }
        if (OB_SUCC(ret) && !found) {
          ret = OB_ENTRY_NOT_EXIST;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_unit_in_group(
    const uint64_t tenant_id,
    const uint64_t unit_group_id,
    const common::ObZone &zone,
    share::ObUnitInfo &unit_info)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnitInfo> unit_info_array;
  if (OB_UNLIKELY(zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_FAIL(get_unit_group(
          tenant_id,
          unit_group_id,
          unit_info_array))) {
    LOG_WARN("fail to get unit group", KR(ret), K(tenant_id), K(unit_group_id));
  } else {
    bool found = false;
    for (int64_t i = 0; !found && OB_SUCC(ret) && i < unit_info_array.count(); ++i) {
      const share::ObUnitInfo &this_unit_info = unit_info_array.at(i);
      if (this_unit_info.unit_.zone_ != zone) {
        // bypass
      } else if (OB_FAIL(unit_info.assign(this_unit_info))) {
        LOG_WARN("fail to assign unit info", KR(ret));
      } else {
        found = true;
      }
    }
    if (OB_FAIL(ret)) {
      // failed
    } else if (!found) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {} // good
  }
  return ret;
}

int ObUnitManager::get_unit_group(
    const uint64_t tenant_id,
    const uint64_t unit_group_id,
    common::ObIArray<share::ObUnitInfo> &unit_info_array)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id)
                         || 0 == unit_group_id
                         || OB_INVALID_ID == unit_group_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(unit_group_id));
  } else {
    ObArray<share::ObResourcePool *> *cur_pool_array = nullptr;
    ret = get_pools_by_tenant(tenant_id, cur_pool_array);
    if (OB_ENTRY_NOT_EXIST == ret) {
      // bypass
    } else if (OB_SUCCESS != ret) {
      LOG_WARN("fail to get unit group", KR(ret), K(tenant_id), K(unit_group_id));
    } else if (OB_UNLIKELY(nullptr == cur_pool_array)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cur pool array ptr is null", KR(ret), K(tenant_id), K(unit_group_id));
    } else {
      ObUnitInfo unit_info;
      for (int64_t i = 0; OB_SUCC(ret) && i < cur_pool_array->count(); ++i) {
        share::ObResourcePool *cur_pool = cur_pool_array->at(i);
        ObArray<share::ObUnit *> *units = nullptr;
        ObUnitConfig *config = nullptr;
        if (OB_UNLIKELY(nullptr == cur_pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("cur pool is null", KR(ret));
        } else if (OB_FAIL(get_unit_config_by_id(cur_pool->unit_config_id_, config))) {
          LOG_WARN("fail to get pool unit config", KR(ret), K(tenant_id), KPC(cur_pool));
        } else if (OB_FAIL(get_units_by_pool(cur_pool->resource_pool_id_, units))) {
          LOG_WARN("fail to get units by pool", KR(ret),
                   "pool_id", cur_pool->resource_pool_id_);
        } else if (OB_UNLIKELY(nullptr == units)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < units->count(); ++j) {
            if (OB_UNLIKELY(nullptr == units->at(j))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("units ptrs is null", KR(ret), K(tenant_id), KPC(cur_pool));
            } else if (units->at(j)->unit_group_id_ != unit_group_id) {
              // unit group id not match
            } else {
              unit_info.reset();
              unit_info.config_ = *config;
              unit_info.unit_ = *(units->at(j));
              if (OB_FAIL(unit_info.pool_.assign(*cur_pool))) {
                LOG_WARN("fail to assign", KR(ret), KPC(cur_pool));
              } else if (OB_FAIL(unit_info_array.push_back(unit_info))) {
                LOG_WARN("fail to push back", K(unit_info));
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (unit_info_array.count() <= 0) {
          ret = OB_ENTRY_NOT_EXIST;
        }
      }
    }
  }
  return ret;
}

// TODO: disable resource pools intersect for one tenant
//       NEED to disable logics to handle resource pool intersect in server_balancer
int ObUnitManager::grant_pools(ObISQLClient &client,
                               common::ObIArray<uint64_t> &new_unit_group_id_array,
                               const lib::Worker::CompatMode compat_mode,
                               const ObIArray<ObResourcePoolName> &pool_names,
                               const uint64_t tenant_id,
                               const bool is_bootstrap,
                               const bool if_not_grant,
                               const bool skip_offline_server)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  const bool grant = true;
  bool intersect = false;
  bool server_enough = true;
  bool is_grant_pool_allowed = false;
  const bool skip_check_enough = is_bootstrap
                                 || skip_offline_server;
  bool unit_num_legal = false;
  int64_t legal_unit_num = -1;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), KR(ret));
  } else if (pool_names.count() <= 0 || !is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_names), K(tenant_id), K(ret));
  } else if (OB_FAIL(check_pool_intersect(tenant_id, pool_names, intersect))) {
    LOG_WARN("check pool intersect failed", K(pool_names), KR(ret));
  } else if (intersect) {
    ret = OB_POOL_SERVER_INTERSECT;
    LOG_USER_ERROR(OB_POOL_SERVER_INTERSECT, to_cstring(pool_names));
    LOG_WARN("resource pool unit server intersect", K(pool_names), KR(ret));
  } else if (!skip_check_enough
      && OB_FAIL(check_server_enough(tenant_id, pool_names, server_enough))) {
    LOG_WARN("fail to check server enough", KR(ret), K(tenant_id), K(pool_names));
  } else if (!server_enough) {
    ret = OB_UNIT_NUM_OVER_SERVER_COUNT;
    LOG_WARN("resource pool unit num over zone server count", K(ret), K(pool_names), K(tenant_id));
  } else if (OB_FAIL(check_expand_zone_resource_allowed_by_old_unit_stat(
          tenant_id, is_grant_pool_allowed))) {
    LOG_WARN("fail to check grant pools allowed by unit stat", KR(ret), K(tenant_id));
  } else if (!is_grant_pool_allowed) {
    ret = OB_OP_NOT_ALLOW;
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "grant pool when pools in shrinking");
  } else if (OB_FAIL(check_expand_zone_resource_allowed_by_new_unit_stat(
          pool_names, is_grant_pool_allowed))) {
    LOG_WARN("fail to check grant pools allowed by unit stat", KR(ret));
  } else if (OB_FAIL(check_tenant_pools_unit_num_legal(
          tenant_id, pool_names, unit_num_legal, legal_unit_num))) {
    LOG_WARN("fail to check pools unit num legal", KR(ret), K(tenant_id), K(pool_names));
  } else if (!unit_num_legal) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("pools belong to one tenant with different unit num are not allowed",
             KR(ret), K(tenant_id), K(pool_names));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "pools belong to one tenant with different unit num are");
  } else if (OB_FAIL(get_tenant_pool_unit_group_id(
          is_bootstrap, grant, tenant_id, legal_unit_num, new_unit_group_id_array))) {
    LOG_WARN("fail to generate new unit group id", KR(ret), K(tenant_id), K(legal_unit_num));
  } else if (OB_FAIL(change_pool_owner(
          client, new_unit_group_id_array, compat_mode, grant,
          pool_names, tenant_id, is_bootstrap, if_not_grant, skip_offline_server))) {
    LOG_WARN("change pool owner failed", KR(ret), K(grant), K(pool_names), K(tenant_id),
                                         K(compat_mode), K(is_bootstrap), K(if_not_grant),
                                         K(skip_offline_server));
  }
  LOG_INFO("grant resource pools to tenant", K(pool_names), K(tenant_id), K(ret), K(is_bootstrap),
                                             K(if_not_grant), K(skip_offline_server));
  return ret;
}

int ObUnitManager::revoke_pools(ObISQLClient &client,
                                common::ObIArray<uint64_t> &new_unit_group_id_array,
                                const ObIArray<ObResourcePoolName> &pool_names,
                                const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  const bool grant = false;
  const lib::Worker::CompatMode dummy_mode = lib::Worker::CompatMode::INVALID;
  bool unit_num_legal = false;
  int64_t legal_unit_num = -1;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pool_names.count() <= 0 || !is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_names), K(tenant_id), K(ret));
  } else if (OB_FAIL(check_tenant_pools_unit_num_legal(
          tenant_id, pool_names, unit_num_legal, legal_unit_num))) {
    LOG_WARN("fail to check pools unit num legal", KR(ret), K(tenant_id), K(pool_names));
  } else if (!unit_num_legal) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("pools belong to one tenant with different unit num are not allowed",
             KR(ret), K(tenant_id), K(pool_names));
  } else if (OB_FAIL(get_tenant_pool_unit_group_id(
          false /* is_bootstrap */, grant, tenant_id,
          legal_unit_num, new_unit_group_id_array))) {
    LOG_WARN("fail to generate new unit group id", KR(ret), K(tenant_id), K(legal_unit_num));
  } else if (OB_FAIL(change_pool_owner(
          client, new_unit_group_id_array, dummy_mode,
          grant, pool_names, tenant_id, false/*is_bootstrap*/))) {
    LOG_WARN("change pool owner failed", K(grant), K(pool_names), K(tenant_id), K(ret));
  }
  LOG_INFO("revoke resource pools from tenant", K(pool_names), K(ret));
  return ret;
}

int ObUnitManager::commit_change_pool_owner(
    common::ObIArray<uint64_t> &new_unit_group_id_array,
    const bool grant,
    const ObIArray<ObResourcePoolName> &pool_names,
    const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (OB_FAIL(inner_commit_change_pool_owner(
          new_unit_group_id_array, grant, pool_names, tenant_id))) {
    LOG_WARN("fail to inner commit change pool owner", K(ret));
  }
  return ret;
}

int ObUnitManager::inner_commit_change_pool_owner(
    common::ObIArray<uint64_t> &new_ug_id_array,
    const bool grant,
    const ObIArray<ObResourcePoolName> &pool_names,
    const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!check_inner_stat())) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pool_names.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool_names is empty", K(pool_names), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id", K(tenant_id), K(ret));
  } else {
    common::ObArray<ObUnit *> zone_sorted_unit_array;
    for (int64_t i = 0; OB_SUCC(ret) && i < pool_names.count(); ++i) {
      share::ObResourcePool *pool = NULL;
      ObArray<ObUnit *> *units = NULL;
      zone_sorted_unit_array.reset();
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        LOG_WARN("resource pool expected to exist", "pool_name", pool_names.at(i), K(ret));
      } else if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", KP(pool), K(ret));
      } else if (pool->is_granted_to_tenant() && pool->tenant_id_ != tenant_id) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("find pool already grant to other tenant", "pool", *pool,
            K(tenant_id), K(ret));
      } else if (pool->unit_count_ != new_ug_id_array.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit num not match", KR(ret),KPC(pool), K(new_ug_id_array));
      } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", K(ret),
                 "resource_pool_id", pool->resource_pool_id_);
      } else if (OB_UNLIKELY(nullptr == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptr is null", KR(ret));
      } else if (OB_FAIL(zone_sorted_unit_array.assign(*units))) {
        LOG_WARN("fail to assign zone unit array", KR(ret));
      } else {
        UnitZoneOrderCmp cmp_operator;
        std::sort(zone_sorted_unit_array.begin(), zone_sorted_unit_array.end(), cmp_operator);
        for (int64_t j = 0; j < zone_sorted_unit_array.count(); ++j) {
          ObUnit *unit = zone_sorted_unit_array.at(j);
          if (OB_UNLIKELY(nullptr == unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", KR(ret));
          } else {
            unit->unit_group_id_ = new_ug_id_array.at(j % pool->unit_count_);
          }
        }
        if (grant) {
          if (pool->tenant_id_ == tenant_id) {
            // possible if alter system reload unit before invoke this function
          } else {
            if (OB_FAIL(insert_tenant_pool(tenant_id, pool))) {
              LOG_WARN("insert_tenant_pool failed", K(tenant_id), K(ret));
            } else {
              pool->tenant_id_ = tenant_id;
            }
          }
        } else {
          if (!pool->is_granted_to_tenant()) {
            // possible if alter system reload unit before invoke this function
          } else {
            if (OB_FAIL(delete_tenant_pool(pool->tenant_id_, pool))) {
              LOG_WARN("delete_tenant_pool failed", "pool tenant_id", pool->tenant_id_, K(ret));
            } else {
              pool->tenant_id_ = OB_INVALID_ID;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_get_pool_ids_of_tenant(const uint64_t tenant_id,
                                                ObIArray<uint64_t> &pool_ids) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(ret));
  } else {
    pool_ids.reuse();
    ObArray<share::ObResourcePool  *> *pools = NULL;
    if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_DEBUG("get_pools_by_tenant failed", K(tenant_id), K(ret));
      } else {
        // just return empty pool_ids
        if (OB_GTS_TENANT_ID != tenant_id) {
          LOG_INFO("tenant doesn't own any pool", K(tenant_id), KR(ret));
        }
        ret = OB_SUCCESS;
      }
    } else if (NULL == pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", KP(pools), K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
        if (NULL == pools->at(i)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool is null", "pool", OB_P(pools->at(i)), K(ret));
        } else if (OB_FAIL(pool_ids.push_back(pools->at(i)->resource_pool_id_))) {
          LOG_WARN("push_back failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_pool_ids_of_tenant(const uint64_t tenant_id,
                                          ObIArray<uint64_t> &pool_ids) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(ret));
  } else if (OB_FAIL(inner_get_pool_ids_of_tenant(tenant_id, pool_ids))) {
    LOG_WARN("fail to inner get pool ids of tenant", K(ret));
  }
  return ret;
}

int ObUnitManager::get_pool_names_of_tenant(const uint64_t tenant_id,
                                            ObIArray<ObResourcePoolName> &pool_names) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(ret));
  } else {
    ObArray<share::ObResourcePool  *> *pools = NULL;
    if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get_pools_by_tenant failed", K(tenant_id), K(ret));
      } else {
        // just return empty pool_ids
        ret = OB_SUCCESS;
        LOG_WARN("tenant doesn't own any pool", K(tenant_id), K(ret));
      }
    } else if (NULL == pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", KP(pools), K(ret));
    } else {
      pool_names.reuse();
      for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
        if (NULL == pools->at(i)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool is null", "pool", OB_P(pools->at(i)), K(ret));
        } else if (OB_FAIL(pool_names.push_back(pools->at(i)->name_))) {
          LOG_WARN("push_back failed", K(ret));
        }
      }
    }
  }
  return ret;
}


int ObUnitManager::get_unit_config_by_pool_name(
    const ObString &pool_name,
    share::ObUnitConfig &unit_config) const
{
  int ret = OB_SUCCESS;
  share::ObResourcePool *pool = NULL;
  ObUnitConfig *config = NULL;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_FAIL(inner_get_resource_pool_by_name(pool_name, pool))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get resource pool by name failed", K(pool_name), K(ret));
    } else {
      ret = OB_RESOURCE_POOL_NOT_EXIST;
      LOG_WARN("pool not exist", K(ret), K(pool_name));
    }
  } else if (NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is null", KP(pool), K(ret));
  } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, config))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_RESOURCE_UNIT_NOT_EXIST;
    }
    LOG_WARN("can not find config for unit",
             "unit_config_id", pool->unit_config_id_,
             K(ret));
  } else if (NULL == config) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("config is null", KP(config), K(ret));
  } else {
    unit_config = *config;
  }
  return ret;
}


int ObUnitManager::get_pools_of_tenant(const uint64_t tenant_id,
                                       ObIArray<share::ObResourcePool > &pools) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(ret));
  } else {
    ObArray<share::ObResourcePool  *> *inner_pools = NULL;
    if (OB_FAIL(get_pools_by_tenant(tenant_id, inner_pools))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get_pools_by_tenant failed", K(tenant_id), K(ret));
      } else {
        // just return empty pool_ids
        ret = OB_SUCCESS;
        LOG_WARN("tenant doesn't own any pool", K(tenant_id), K(ret));
      }
    } else if (NULL == inner_pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("inner_pools is null", KP(inner_pools), K(ret));
    } else {
      pools.reuse();
      for (int64_t i = 0; OB_SUCC(ret) && i < inner_pools->count(); ++i) {
        if (NULL == inner_pools->at(i)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("inner_pool is null", "inner_pool", OB_P(inner_pools->at(i)), K(ret));
        } else if (OB_FAIL(pools.push_back(*inner_pools->at(i)))) {
          LOG_WARN("push_back failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_zones_of_pools(const ObIArray<ObResourcePoolName> &pool_names,
                                      ObIArray<ObZone> &zones) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pool_names.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool_names is empty", K(pool_names), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pool_names.count(); ++i) {
      share::ObResourcePool  *pool = NULL;
      if (pool_names.at(i).is_empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool name");
        LOG_WARN("invalid pool name", "pool name", pool_names.at(i), K(ret));
      } else if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        if (OB_ENTRY_NOT_EXIST != ret) {
          LOG_WARN("get resource pool by name failed", "name", pool_names.at(i), K(ret));
        } else {
          ret = OB_RESOURCE_POOL_NOT_EXIST;
          LOG_USER_ERROR(OB_RESOURCE_POOL_NOT_EXIST, to_cstring(pool_names.at(i)));
          LOG_WARN("pool not exist", "pool_name", pool_names.at(i), K(ret));
        }
      } else if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", KP(pool), K(ret));
      } else if (OB_FAIL(append(zones, pool->zone_list_))) {
        LOG_WARN("append failed", "zone_list", pool->zone_list_, K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::get_pool_id(const share::ObResourcePoolName &pool_name,
    uint64_t &pool_id) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pool_name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid pool_name", K(pool_name), K(ret));
  } else {
    share::ObResourcePool  *pool = NULL;
    if (OB_FAIL(inner_get_resource_pool_by_name(pool_name, pool))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get resource pool by name failed", K(pool_name), K(ret));
      } else {
        ret = OB_RESOURCE_POOL_NOT_EXIST;
        LOG_WARN("pool not exist", K(ret), K(pool_name));
      }
    } else if (NULL == pool) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("pool is null", KP(pool), K(ret));
    } else {
      pool_id = pool->resource_pool_id_;
    }
  }
  return ret;
}

int ObUnitManager::get_pools(common::ObIArray<share::ObResourcePool> &pools) const
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else {
    ObHashMap<uint64_t, share::ObResourcePool  *>::const_iterator iter = id_pool_map_.begin();
    for ( ; OB_SUCC(ret) && iter != id_pool_map_.end(); ++iter) {
      if (NULL == iter->second) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("iter->second is null", KP(iter->second), K(ret));
      } else if (OB_FAIL(pools.push_back(*(iter->second)))) {
        LOG_WARN("push_back failed", K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::create_sys_units(const ObIArray<ObUnit> &sys_units)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (sys_units.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("sys_units is empty", K(sys_units), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sys_units.count(); ++i) {
      if (OB_FAIL(add_unit(*proxy_, sys_units.at(i)))) {
        LOG_WARN("add_unit failed", "unit", sys_units.at(i), K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_get_tenant_pool_zone_list(
    const uint64_t tenant_id,
    common::ObIArray<common::ObZone> &zone_list) const
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *pools = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(NULL == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools ptr is null", K(ret), K(tenant_id));
  } else if (pools->count() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools array is empty", K(ret), K(*pools));
  } else {
    zone_list.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
      const share::ObResourcePool *pool = pools->at(i);
      if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret), KP(pool));
      } else if (OB_FAIL(append(zone_list, pool->zone_list_))) {
        LOG_WARN("fail to append", K(ret));
      } else {} // no more to do
    }
  }
  return ret;
}

int ObUnitManager::get_tenant_pool_zone_list(
    const uint64_t tenant_id,
    common::ObIArray<common::ObZone> &zone_list) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(inner_get_tenant_pool_zone_list(tenant_id, zone_list))) {
    LOG_WARN("fail to inner get tenant pool zone list", K(ret));
  }
  return ret;
}

int ObUnitManager::cancel_migrate_out_units(const ObAddr &server)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  ObArray<uint64_t> migrate_units;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(server), K(ret));
  } else if (OB_FAIL(get_migrate_units_by_server(server, migrate_units))) {
    LOG_WARN("get_migrate_units_by_server failed", K(server), K(ret));
  } else {
    const EndMigrateOp op = REVERSE;
    for (int64_t i = 0; OB_SUCC(ret) && i < migrate_units.count(); ++i) {
      if (OB_FAIL(end_migrate_unit(migrate_units.at(i), op))) {
        LOG_WARN("end_migrate_unit failed", "unit_id", migrate_units.at(i), K(op), K(ret));
      }
    }
  }
  LOG_INFO("cancel migrate out units", K(server), K(ret));
  return ret;
}

int ObUnitManager::check_server_empty(const ObAddr &server, bool &empty) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  ObArray<ObUnitLoad> *loads = NULL;
  empty = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server", K(server), K(ret));
  } else if (OB_FAIL(get_loads_by_server(server, loads))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_loads_by_server failed", K(server), K(ret));
    } else {
      ret = OB_SUCCESS;
      empty = true;
    }
  } else if (NULL == loads) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("loads is null", KP(loads), K(ret));
  } else {
    empty = false;
  }
  return ret;
}

int ObUnitManager::finish_migrate_unit_not_in_tenant(
                   share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  //Not in tenant unit
  //Using pool, take the unit corresponding to the pool whose tenant_id is -1
  if (-1 != pool->tenant_id_) {
    //in tenant
    //ignore in tenant unit
  } else {
    ObArray<ObUnitInfo> unit_infos;
    if (OB_FAIL(inner_get_unit_infos_of_pool(pool->resource_pool_id_, unit_infos))) {
      LOG_WARN("fail to get units by pool", K(ret));
    } else {
      FOREACH_CNT_X(unit_info, unit_infos, OB_SUCC(ret)) {
        if ((*unit_info).unit_.migrate_from_server_.is_valid()) {
          if (OB_INVALID_ID == (*unit_info).unit_.unit_id_) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid argument", K((*unit_info).unit_.unit_id_), K(ret));
          } else {
            const EndMigrateOp op = COMMIT;
            if (OB_FAIL(end_migrate_unit((*unit_info).unit_.unit_id_, op))) {
              LOG_WARN("end migrate unit failed", K(ret), K((*unit_info).unit_.unit_id_), K(op));
            } else {
              LOG_INFO("finish migrate unit not in tenant", K(ret),
                       "unit_id", (*unit_info).unit_.unit_id_);
            }
          }
        } else {}//ignore not in migrate unit
      }
    }
  }
  return ret;
}

int ObUnitManager::finish_migrate_unit_not_in_locality(
                   uint64_t tenant_id,
                   share::schema::ObSchemaGetterGuard *schema_guard,
                   ObArray<common::ObZone> zone_list)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  const ObTenantSchema *tenant_schema = NULL;
  ObArray<ObUnitInfo> unit_infos;
  ObArray<common::ObZone> zone_locality_list;
  if (OB_FAIL((*schema_guard).get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant schema", K(ret), K(tenant_id));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant schema is null", K(ret));
  } else if (OB_FAIL(tenant_schema->get_zone_list(zone_locality_list))) {
    LOG_WARN("fail to get zone replica attr array", K(ret));
  } else {
    for (int64_t i = 0; i < zone_list.count() && OB_SUCC(ret); i++) {
      if (!has_exist_in_array(zone_locality_list, zone_list.at(i))) {
        //Get the unit that is in the zone locality but not in the zone_list,
        //the zone that is not in the locality
        if (OB_FAIL(inner_get_zone_alive_unit_infos_by_tenant(
                    tenant_id, zone_list.at(i), unit_infos))) {
          LOG_WARN("fail to get zone alive unit infos by tenant", K(ret));
        } else {
          FOREACH_CNT_X(unit_info, unit_infos, OB_SUCC(ret)) {
            if ((*unit_info).unit_.migrate_from_server_.is_valid()) {
              if (OB_INVALID_ID == (*unit_info).unit_.unit_id_) {
                ret = OB_INVALID_ARGUMENT;
                LOG_WARN("invalid argument", K((*unit_info).unit_.unit_id_), K(ret));
              } else {
                const EndMigrateOp op = COMMIT;
                if (OB_FAIL(end_migrate_unit((*unit_info).unit_.unit_id_, op))) {
                  LOG_WARN("end migrate unit failed", K(ret),
                            K((*unit_info).unit_.unit_id_), K(op));
                } else {
                  LOG_INFO("finish migrate unit not in locality", K(ret),
                           "unit_id", (*unit_info).unit_.unit_id_);
                }
              }
            } else {} //ignore not in migrate unit
          } //end FOREACH
        }
      } else {} //ignore in locality unit
    } //end for
  }
  return ret;
}

int ObUnitManager::finish_migrate_unit(const uint64_t unit_id)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(unit_id), K(ret));
  } else {
    const EndMigrateOp op = COMMIT;
    if (OB_FAIL(end_migrate_unit(unit_id, op))) {
      LOG_WARN("end_migrate_unit failed", K(unit_id), K(op), K(ret));
    }
  }
  LOG_INFO("finish migrate unit", K(unit_id), K(ret));
  return ret;
}

int ObUnitManager::get_zone_active_unit_infos_by_tenant(
    const uint64_t tenant_id,
    const common::ObZone &zone,
    common::ObIArray<share::ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> rs_pool;
  ObArray<ObUnitInfo> unit_array;
  unit_infos.reset();
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))
      || OB_UNLIKELY(zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(zone));
  } else if (OB_FAIL(get_pool_ids_of_tenant(tenant_id, rs_pool))) {
    LOG_WARN("fail to get pool ids by tennat", K(ret), K(tenant_id));
  } else {
    FOREACH_X(pool, rs_pool, OB_SUCCESS == ret) {
      unit_array.reuse();
      if (!check_inner_stat()) {
        ret = OB_INNER_STAT_ERROR;
        LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
      } else if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret));
      } else if (OB_FAIL(get_unit_infos_of_pool(*pool, unit_array))) {
        LOG_WARN("fail to get unit infos of pool", K(ret));
      } else if (unit_array.count() > 0) {
        FOREACH_X(u, unit_array, OB_SUCCESS == ret) {
          if (OB_UNLIKELY(NULL == u)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit is empty", K(ret));
          } else if (zone != u->unit_.zone_) {
            // do not belong to this zone
          } else if (ObUnit::UNIT_STATUS_ACTIVE != u->unit_.status_) {
            // ignore the unit which is in deleting status
          } else if (OB_FAIL(unit_infos.push_back(*u))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      } else {} // empty array
    }
  }
  return ret;
}

int ObUnitManager::inner_get_zone_alive_unit_infos_by_tenant(
    const uint64_t tenant_id,
    const common::ObZone &zone,
    common::ObIArray<share::ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> rs_pool;
  ObArray<ObUnitInfo> unit_array;
  unit_infos.reset();
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))
      || OB_UNLIKELY(zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(zone));
  } else if (OB_FAIL(inner_get_pool_ids_of_tenant(tenant_id, rs_pool))) {
    LOG_WARN("fail to get pool ids by tennat", K(ret), K(tenant_id));
  } else {
    FOREACH_X(pool, rs_pool, OB_SUCCESS == ret) {
      unit_array.reuse();
      if (!check_inner_stat()) {
        ret = OB_INNER_STAT_ERROR;
        LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
      } else if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret));
      } else if (OB_FAIL(inner_get_unit_infos_of_pool(*pool, unit_array))) {
        LOG_WARN("fail to get unit infos of pool", K(ret));
      } else if (unit_array.count() > 0) {
        FOREACH_X(u, unit_array, OB_SUCCESS == ret) {
          bool is_alive = false;
          bool is_in_service = false;
          if (OB_UNLIKELY(NULL == u)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit is empty", K(ret));
          } else if (zone != u->unit_.zone_) {
            // do not belong to this zone
            } else if (OB_FAIL(server_mgr_.check_server_alive(u->unit_.server_, is_alive))) {
            LOG_WARN("check_server_alive failed", "server", u->unit_.server_, K(ret));
          } else if (OB_FAIL(server_mgr_.check_in_service(u->unit_.server_, is_in_service))) {
            LOG_WARN("check server in service failed", "server", u->unit_.server_, K(ret));
          } else if (!is_alive || !is_in_service) {
            // ignore unit on not-alive server
            } else if (ObUnit::UNIT_STATUS_ACTIVE != u->unit_.status_) {
            // ignore the unit which is in deleting status
            } else if (OB_FAIL(unit_infos.push_back(*u))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      } else {} // empty array
    }
  }
  return ret;
}

int ObUnitManager::get_zone_alive_unit_infos_by_tenant(
    const uint64_t tenant_id,
    const common::ObZone &zone,
    common::ObIArray<share::ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(ret));
  } else if (OB_FAIL(inner_get_zone_alive_unit_infos_by_tenant(tenant_id, zone, unit_infos))) {
    LOG_WARN("fail to inner get zone alive unit infos by tenant", K(ret));
  }
  return ret;
}

int ObUnitManager::get_all_unit_infos_by_tenant(const uint64_t tenant_id,
                                                ObIArray<ObUnitInfo> &unit_infos)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  unit_infos.reset();
  common::ObArray<common::ObZone> tenant_zone_list;
  ObArray<uint64_t> rs_pool;
  ObArray<ObUnitInfo> unit_array;

  if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_ISNULL(schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service is null", K(schema_service_), KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(tenant_id, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(NULL == tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant schema is null", K(ret), KP(tenant_schema));
  } else if (OB_FAIL(tenant_schema->get_zone_list(tenant_zone_list))) {
    LOG_WARN("fail to get zone list", K(ret));
  } else if (OB_FAIL(get_pool_ids_of_tenant(tenant_id, rs_pool))) {
    LOG_WARN("fail to get pool ids of tenant", K(ret), K(tenant_id));
  } else {
    FOREACH_X(pool, rs_pool, OB_SUCCESS == ret) {
      unit_array.reuse();
      if (!check_inner_stat()) {
        ret = OB_INNER_STAT_ERROR;
        LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
      } else if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret));
      } else if (OB_FAIL(get_unit_infos_of_pool(*pool, unit_array))) {
        LOG_WARN("fail to get unit infos of pool", K(ret));
      } else if (unit_array.count() > 0) {
        FOREACH_X(u, unit_array, OB_SUCCESS == ret) {
          if (OB_UNLIKELY(NULL == u)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("empty is null", K(ret));
          } else if (!has_exist_in_array(tenant_zone_list, u->unit_.zone_)) {
            // this unit do not in tenant zone list, ignore
          } else if (OB_FAIL(unit_infos.push_back(*u))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      } else {} // do nothing
    }
  }
  return ret;
}

int ObUnitManager::get_active_unit_infos_by_tenant(const uint64_t tenant_id,
                                                   ObIArray<ObUnitInfo> &unit_infos)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  unit_infos.reset();
  if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(nullptr == schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service ptr is null", K(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(
          tenant_id, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(NULL == tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant schema is null", K(ret), KP(tenant_schema));
  } else if (OB_FAIL(get_active_unit_infos_by_tenant(*tenant_schema, unit_infos))) {
    LOG_WARN("fail to get active unit infos", K(ret), K(tenant_id));
  }
  return ret;
}

int ObUnitManager::get_active_unit_infos_by_tenant(
    const share::schema::ObTenantSchema &tenant_schema,
    ObIArray<ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = tenant_schema.get_tenant_id();
  common::ObArray<common::ObZone> tenant_zone_list;
  ObArray<uint64_t> rs_pool;
  ObArray<ObUnitInfo> unit_array;
  unit_infos.reset();
  if (OB_FAIL(tenant_schema.get_zone_list(tenant_zone_list))) {
    LOG_WARN("fail to get zone list", K(ret));
  } else if (OB_FAIL(get_pool_ids_of_tenant(tenant_id, rs_pool))) {
    LOG_WARN("fail to get pool ids of tenant", K(ret), K(tenant_id));
  } else {
    FOREACH_X(pool, rs_pool, OB_SUCCESS == ret) {
      unit_array.reuse();
      if (!check_inner_stat()) {
        ret = OB_INNER_STAT_ERROR;
        LOG_WARN("check inner stat failed", K(ret), K(inited_), K(loaded_));
      } else if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", K(ret));
      } else if (OB_FAIL(get_unit_infos_of_pool(*pool, unit_array))) {
        LOG_WARN("fail to get unit infos of pool", K(ret));
      } else if (unit_array.count() > 0) {
        FOREACH_X(u, unit_array, OB_SUCCESS == ret) {
          if (OB_UNLIKELY(NULL == u)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("empty is null", K(ret));
          } else if (!has_exist_in_array(tenant_zone_list, u->unit_.zone_)) {
            // this unit do not in tenant zone list, ignore
          } else if (ObUnit::UNIT_STATUS_ACTIVE != u->unit_.status_) {
            // ignore the unit which is not in active status
          } else if (OB_FAIL(unit_infos.push_back(*u))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      } else {} // do nothing
    }
  }
  return ret;
}

int ObUnitManager::get_zone_active_unit_infos_by_tenant(
    const uint64_t tenant_id,
    const common::ObIArray<common::ObZone> &tenant_zone_list,
    ObIArray<ObUnitInfo> &unit_info) const
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> pool_ids;
  ObArray<share::ObResourcePool  *> *pools = NULL;
  share::schema::ObSchemaGetterGuard schema_guard;
  unit_info.reset();
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (0 >= tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant_id is invalid", K(ret), K(tenant_id));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_pools_by_tenant failed", K(tenant_id), K(ret));
    } else {
      // just return empty pool_ids
      ret = OB_SUCCESS;
      LOG_WARN("tenant doesn't own any pool", K(tenant_id), K(ret));
    }
  } else if (NULL == pools) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools is null", KP(pools), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
      if (NULL == pools->at(i)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", "pool", OB_P(pools->at(i)), K(ret));
      } else if (OB_FAIL(pool_ids.push_back(pools->at(i)->resource_pool_id_))) {
        LOG_WARN("push_back failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObArray<ObUnitInfo> unit_in_pool;
    for (int64_t i = 0; i < pool_ids.count() && OB_SUCC(ret); i++) {
      uint64_t pool_id = pool_ids.at(i);
      unit_in_pool.reset();
      if (OB_FAIL(inner_get_unit_infos_of_pool(pool_id, unit_in_pool))) {
        LOG_WARN("fail to inner get unit infos", K(ret), K(pool_id));
      } else {
        for (int64_t j = 0; j < unit_in_pool.count() && OB_SUCC(ret); j++) {
          if (ObUnit::UNIT_STATUS_ACTIVE != unit_in_pool.at(j).unit_.status_) {
            //nothing todo
          } else if (!has_exist_in_array(tenant_zone_list, unit_in_pool.at(j).unit_.zone_)) {
            //nothing todo
          } else if (OB_FAIL(unit_info.push_back(unit_in_pool.at(j)))) {
            LOG_WARN("fail to push back", K(ret), K(unit_in_pool), K(j));
          }
        }
      }
    }
  }
  return ret;
}


int ObUnitManager::commit_shrink_resource_pool(
    const uint64_t resource_pool_id)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(lock_);
  common::ObMySQLTransaction trans;
  share::ObResourcePool *pool = NULL;
  ObArray<ObUnit *> *units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(loaded_), K(inited_));
  } else if (OB_UNLIKELY(OB_INVALID_ID == resource_pool_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(resource_pool_id));
  } else if (OB_FAIL(get_resource_pool_by_id(resource_pool_id, pool))) {
    LOG_WARN("fail to get resource pool", K(ret), K(resource_pool_id));
  } else if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool ptr is null", K(ret), KP(pool));
  } else if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
    LOG_WARN("fail to get units by pool", K(ret), K(resource_pool_id));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool ptr is null", K(ret), KP(units));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("start transaction failed", K(ret));
  } else if (OB_FAIL(complete_shrink_pool_unit_num_rs_job(pool->resource_pool_id_, pool->tenant_id_, trans))) {
    LOG_WARN("do rs_job failed", K(ret));
  } else {
    // Commit shrink resource pool only needs to change the state of the unit,
    // the state of the resource pool itself has been changed before,
    // and there is no need to adjust it again.
    common::ObArray<uint64_t> unit_ids;
    int64_t unit_count = pool->unit_count_;
    if (unit_count <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit count unexpected", K(ret), K(unit_count));
    } else if (units->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("zone unit ptrs has no element", K(ret), "unit_cnt", units->count());
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
        const ObUnit *unit = units->at(i);
        if (OB_UNLIKELY(NULL == unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret), KP(unit), K(i));
        } else if (ObUnit::UNIT_STATUS_DELETING == unit->status_) {
          if (OB_FAIL(ut_operator_.remove_unit(trans, *unit))) {
            LOG_WARN("fail to remove unit", K(ret), "unit", *unit);
          } else if (OB_FAIL(unit_ids.push_back(unit->unit_id_))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        } else {} // active unit, do nothing
      }
    }
    // however, we need to end this transaction
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("trans end failed", K(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(delete_inmemory_units(pool->resource_pool_id_, unit_ids))) {
      LOG_WARN("fail to delete unit groups", K(ret));
    } else {} // no more to do
  }
  return ret;
}

int ObUnitManager::get_deleting_units_of_pool(
    const uint64_t resource_pool_id,
    ObIArray<share::ObUnit> &units) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  ObArray<ObUnit *> *inner_units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(loaded_), K(inited_));
  } else if (OB_UNLIKELY(OB_INVALID_ID == resource_pool_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(resource_pool_id));
  } else if (OB_FAIL(get_units_by_pool(resource_pool_id, inner_units))) {
      LOG_WARN("fail to get units by pool", K(ret), K(resource_pool_id));
  } else if (NULL == inner_units) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("inner_units ptr is null", K(ret), KP(inner_units));
  } else {
    units.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < inner_units->count(); ++i) {
      const share::ObUnit *this_unit = inner_units->at(i);
      if (NULL == this_unit) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret), KP(this_unit));
      } else if (ObUnit::UNIT_STATUS_DELETING == this_unit->status_) {
        if (OB_FAIL(units.push_back(*this_unit))) {
          LOG_WARN("fail to push back", K(ret));
        } else {} // no more to do
      } else if (ObUnit::UNIT_STATUS_ACTIVE == this_unit->status_) {
        // a normal unit, ignore
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected unit status", K(ret), "unit_status", this_unit->status_);
      }
    }
    LOG_INFO("get deleting units of pool", K(ret), K(resource_pool_id), K(units));
  }
  return ret;
}

int ObUnitManager::get_unit_infos_of_pool(const uint64_t resource_pool_id,
                                          ObIArray<ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(resource_pool_id), K(ret));
  } else if (OB_FAIL(inner_get_unit_infos_of_pool(resource_pool_id, unit_infos))) {
    LOG_WARN("inner_get_unit_infos_of_pool failed", K(resource_pool_id), K(ret));
  }
  return ret;
}

int ObUnitManager::get_active_unit_infos_of_pool(const uint64_t resource_pool_id,
                                                 common::ObIArray<share::ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  common::ObArray<share::ObUnitInfo> tmp_unit_infos;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(resource_pool_id));
  } else if (OB_FAIL(inner_get_unit_infos_of_pool(resource_pool_id, tmp_unit_infos))) {
    LOG_WARN("inner_get_unit_infos_of_pool failed", K(ret), K(resource_pool_id));
  } else {
    for (int64_t i = 0; i < tmp_unit_infos.count() && OB_SUCC(ret); ++i) {
      const share::ObUnitInfo &this_unit_info = tmp_unit_infos.at(i);
      if (ObUnit::UNIT_STATUS_ACTIVE == this_unit_info.unit_.status_) {
        if (OB_FAIL(unit_infos.push_back(this_unit_info))) {
          LOG_WARN("fail to push back", K(ret));
        } else {} // no more to do
      } else {} // ignore unit whose status is not active
    }
  }
  return ret;
}

int ObUnitManager::inner_get_unit_infos_of_pool(const uint64_t resource_pool_id,
                                                ObIArray<ObUnitInfo> &unit_infos) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(resource_pool_id), K(ret));
  } else {
    unit_infos.reuse();
    ObUnitInfo unit_info;
    share::ObResourcePool  *pool = NULL;
    ObUnitConfig *config = NULL;
    if (OB_FAIL(get_resource_pool_by_id(resource_pool_id, pool))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_RESOURCE_POOL_NOT_EXIST;
      }
      LOG_WARN("get_resource_pool_by_id failed", K(resource_pool_id), K(ret));
    } else if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool is null", KP(pool), K(ret));
    } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, config))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_RESOURCE_UNIT_NOT_EXIST;
      }
      LOG_WARN("get_unit_config_by_id failed", "unit config id", pool->unit_config_id_, K(ret));
    } else if (NULL == config) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("config is null", KP(config), K(ret));
    } else {
      unit_info.reset();
      if (OB_FAIL(unit_info.pool_.assign(*pool))) {
        LOG_WARN("failed to assign unit_info.pool_", K(ret));
      } else {
        unit_info.config_ = *config;
        ObArray<ObUnit *> *units = NULL;
        if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
          LOG_WARN("get_units_by_pool failed", K(resource_pool_id), K(ret));
        } else if (NULL == units) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("units is null", KP(units), K(ret));
        } else if (units->count() <= 0) {
          ret = OB_ENTRY_NOT_EXIST;
          LOG_WARN("units of resource pool not exist", K(resource_pool_id), K(ret));
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
            if (NULL == units->at(i)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("unit is null", "unit", OB_P(units->at(i)), K(ret));
            } else {
              unit_info.unit_ = *units->at(i);
              if (OB_FAIL(unit_infos.push_back(unit_info))) {
                LOG_WARN("push_back failed", K(ret));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_get_unit_info_by_id(const uint64_t unit_id, ObUnitInfo &unit_info) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(unit_id), K(ret));
  } else {
    ObUnit *unit = NULL;
    ObUnitConfig *config = NULL;
    share::ObResourcePool  *pool = NULL;
    if (OB_FAIL(get_unit_by_id(unit_id, unit))) {
      LOG_WARN("get_unit_by_id failed", K(unit_id), K(ret));
    } else if (NULL == unit) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit is null", KP(unit), K(ret));
    } else if (OB_FAIL(get_resource_pool_by_id(unit->resource_pool_id_, pool))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_RESOURCE_POOL_NOT_EXIST;
      }
      LOG_WARN("get_resource_pool_by_id failed", "pool id", unit->resource_pool_id_, K(ret));
    } else if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool is null", KP(pool), K(ret));
    } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, config))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_RESOURCE_UNIT_NOT_EXIST;
      }
      LOG_WARN("get_unit_config_by_id failed", "unit config id", pool->unit_config_id_, K(ret));
    } else if (NULL == config) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("config is null", KP(config), K(ret));
    } else {
      if (OB_FAIL(unit_info.pool_.assign(*pool))) {
        LOG_WARN("failed to assign unit_info.pool_", K(ret));
      } else {
        unit_info.unit_ = *unit;
        unit_info.config_ = *config;
      }
    }
  }
  return ret;
}

int ObUnitManager::get_unit(const uint64_t tenant_id, const ObAddr &server,
                            share::ObUnit &unit) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id) || !server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id or invalid server", K(tenant_id), K(server), K(ret));
  } else {
    ObArray<share::ObResourcePool  *> *pools = NULL;
    if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
      LOG_WARN("get_pools_by_tenant failed", K(tenant_id), K(ret));
    } else if (NULL == pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", KP(pools), K(ret));
    } else if (pools->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is empty", "pools", *pools, K(ret));
    } else {
      bool find = false;
      FOREACH_CNT_X(pool, *pools, OB_SUCCESS == ret && !find) {
        if (NULL == *pool) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("entry in pools is null", K(ret));
        } else {
          ObArray<ObUnit *> *units = NULL;
          if (OB_FAIL(get_units_by_pool((*pool)->resource_pool_id_, units))) {
            LOG_WARN("get_units_by_pool failed", "pool id", (*pool)->resource_pool_id_, K(ret));
          } else if (NULL == units) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("units is null", KP(units), K(ret));
          } else if (units->count() <= 0) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("units is empty", "units", *units, K(ret));
          } else {
            FOREACH_CNT_X(inner_unit, *units, OB_SUCCESS == ret && !find) {
              if (NULL == *inner_unit) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("entry in units is null", "unit", OB_P(*inner_unit), K(ret));
              } else if ((*inner_unit)->server_ == server
                  || (*inner_unit)->migrate_from_server_ == server) {
                unit = *(*inner_unit);
                find = true;
              }
            }
          }
        }
      }
      if (OB_SUCCESS == ret && !find) {
        ret = OB_ENTRY_NOT_EXIST;
        LOG_WARN("unit not exist", K(tenant_id), K(server), K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::extract_unit_ids(
    const common::ObIArray<share::ObUnit *> &units,
    common::ObIArray<uint64_t> &unit_ids)
{
  int ret = OB_SUCCESS;
  unit_ids.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < units.count(); ++i) {
    if (OB_UNLIKELY(NULL == units.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit ptr is null", K(ret));
    } else if (OB_FAIL(unit_ids.push_back(units.at(i)->unit_id_))) {
      LOG_WARN("fail to push back", K(ret));
    } else {} // no more to do
  }
  return ret;
}

int ObUnitManager::inner_get_unit_ids(ObIArray<uint64_t> &unit_ids) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  }
  for (ObHashMap<uint64_t, ObArray<ObUnit *> *>::const_iterator it = pool_unit_map_.begin();
      OB_SUCCESS == ret && it != pool_unit_map_.end(); ++it) {
    if (NULL == it->second) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("pointer of ObArray<ObUnit *> is null", "pool_id", it->first, K(ret));
    } else if (it->second->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("array of unit is empty", "pool_id", it->first, K(ret));
    } else {
      const ObArray<ObUnit *> units = *it->second;
      for (int64_t i = 0; OB_SUCC(ret) && i < units.count(); ++i) {
        uint64_t unit_id = units.at(i)->unit_id_;
        if (OB_FAIL(unit_ids.push_back(unit_id))) {
          LOG_WARN("fail push back it", K(unit_id), K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_unit_ids(ObIArray<uint64_t> &unit_ids) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(inner_get_unit_ids(unit_ids))) {
    LOG_WARN("fail to inner get unit ids", K(ret));
  }
  return ret;
}

int ObUnitManager::calc_sum_load(const ObArray<ObUnitLoad> *unit_loads,
                                 ObUnitConfig &sum_load)
{
  int ret = OB_SUCCESS;
  sum_load.reset();
  if (NULL == unit_loads) {
    // all be zero
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < unit_loads->count(); ++i) {
      if (!unit_loads->at(i).is_valid()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid unit_load", "unit_load", unit_loads->at(i), K(ret));
      } else {
        sum_load += *unit_loads->at(i).unit_config_;
      }
    }
  }
  return ret;
}

int ObUnitManager::check_resource_pool(share::ObResourcePool  &resource_pool) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (resource_pool.name_.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "resource pool name");
    LOG_WARN("invalid resource pool name", "resource pool name", resource_pool.name_, K(ret));
  } else if (resource_pool.unit_count_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unit num");
    LOG_WARN("invalid resource unit num", "unit num", resource_pool.unit_count_, K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < resource_pool.zone_list_.count(); ++i) {
      for (int64_t j = i + 1; OB_SUCC(ret) && j < resource_pool.zone_list_.count(); ++j) {
        if (resource_pool.zone_list_[i] == resource_pool.zone_list_[j]) {
          ret = OB_ZONE_DUPLICATED;
          LOG_USER_ERROR(OB_ZONE_DUPLICATED, to_cstring(resource_pool.zone_list_[i]),
              to_cstring(resource_pool.zone_list_));
          LOG_WARN("duplicate zone in zone list", "zone_list", resource_pool.zone_list_, K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      FOREACH_CNT_X(zone, resource_pool.zone_list_, OB_SUCCESS == ret) {
        bool zone_exist = false;
        if (OB_FAIL(zone_mgr_.check_zone_exist(*zone, zone_exist))) {
          LOG_WARN("check_zone_exist failed", K(zone), K(ret));
        } else if (!zone_exist) {
          ret = OB_ZONE_INFO_NOT_EXIST;
          LOG_USER_ERROR(OB_ZONE_INFO_NOT_EXIST, to_cstring(*zone));
          LOG_WARN("zone not exist", "zone", *zone, K(ret));
        }
      }
    }
    if (OB_SUCCESS == ret && 0 == resource_pool.zone_list_.count()) {
      ObArray<ObZoneInfo> zone_infos;
      if (OB_FAIL(zone_mgr_.get_zone(zone_infos))) {
        LOG_WARN("get_zone failed", K(ret));
      } else {
        FOREACH_CNT_X(zone_info, zone_infos, OB_SUCCESS == ret) {
          if (OB_FAIL(resource_pool.zone_list_.push_back(zone_info->zone_))) {
            LOG_WARN("push_back failed", K(ret));
          }
        }
        if (OB_SUCCESS == ret && resource_pool.zone_list_.count() <= 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("not active zone found", K(ret));
        }
      }
    }
    FOREACH_CNT_X(zone, resource_pool.zone_list_, OB_SUCCESS == ret) {
      int64_t alive_server_count = 0;
      if (OB_FAIL(server_mgr_.get_alive_server_count(*zone, alive_server_count))) {
        LOG_WARN("get_alive_servers failed", "zone", *zone, K(ret));
      } else if (alive_server_count < resource_pool.unit_count_) {
        ret = OB_UNIT_NUM_OVER_SERVER_COUNT;
        LOG_WARN("resource pool unit num over zone server count", "unit_count",
            resource_pool.unit_count_, K(alive_server_count), K(ret), "zone", *zone);
      }
    }
  }
  return ret;
}

int ObUnitManager::allocate_new_pool_units_(
    ObISQLClient &client,
    const share::ObResourcePool  &pool,
    const char *module)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!pool.is_valid() || pool.zone_list_.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid pool", K(pool), K(ret));
  } else {
    const bool new_allocate_pool = true;
    const int64_t delta_unit_num = pool.unit_count_;
    ObArray<ObAddr> new_servers;
    if (OB_FAIL(allocate_pool_units_(
            client, pool, pool.zone_list_, nullptr, new_allocate_pool, delta_unit_num, module, new_servers))) {
      LOG_WARN("allocate pool units failed", K(module), KR(ret), K(delta_unit_num), K(pool));
    }
  }
  return ret;
}

int ObUnitManager::try_notify_tenant_server_unit_resource(
    const uint64_t tenant_id,
    const bool is_delete,
    ObNotifyTenantServerResourceProxy &notify_proxy,
    const share::ObResourcePool &new_pool,
    const lib::Worker::CompatMode compat_mode,
    const share::ObUnit &unit,
    const bool if_not_grant,
    const bool skip_offline_server)
{
  int ret = OB_SUCCESS;
  bool is_server_alive = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(nullptr == srv_rpc_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("srv_rpc_proxy_ ptr is null", K(ret));
  } else if (OB_FAIL(server_mgr_.check_server_alive(unit.server_, is_server_alive))) {
    LOG_WARN("fail to check server alive", K(ret), "server", unit.server_);
  } else if (!is_server_alive && (is_delete || skip_offline_server)) {
    LOG_INFO("server not alive when delete unit, ignore", "server", unit.server_);
  } else if (!is_delete && OB_FAIL(server_mgr_.set_with_partition(unit.server_))) {
    LOG_WARN("failed to set with partition", KR(ret), K(unit));
  } else {
    share::ObUnitConfig *unit_config = nullptr;
    if (!is_valid_tenant_id(new_pool.tenant_id_) && !is_delete) {
      // bypass
    } else if (OB_FAIL(get_unit_config_by_id(new_pool.unit_config_id_, unit_config))) {
      LOG_WARN("fail to get unit config by id", K(ret));
    } else if (OB_UNLIKELY(nullptr == unit_config)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit config is null", K(ret), "unit_config_id", new_pool.unit_config_id_);
    } else {
      obrpc::TenantServerUnitConfig tenant_unit_server_config;
      int64_t start = ObTimeUtility::current_time();
      int64_t rpc_timeout = NOTIFY_RESOURCE_RPC_TIMEOUT;
      if (INT64_MAX != THIS_WORKER.get_timeout_ts()) {
        rpc_timeout = max(rpc_timeout, THIS_WORKER.get_timeout_remain());
      }
      if (OB_FAIL(tenant_unit_server_config.init(
              tenant_id, unit.unit_id_, compat_mode, *unit_config,
              ObReplicaType::REPLICA_TYPE_FULL, if_not_grant, is_delete))) {
        LOG_WARN("fail to init server config", KR(ret), K(tenant_id));
      } else if (OB_FAIL(notify_proxy.call(unit.server_, rpc_timeout, tenant_unit_server_config))) {
        LOG_WARN("fail to call notify resource to server",
                 K(ret), K(rpc_timeout), "dst", unit.server_);
        if (OB_TENANT_EXIST == ret) {
          ret = OB_TENANT_RESOURCE_UNIT_EXIST;
          LOG_USER_ERROR(OB_TENANT_RESOURCE_UNIT_EXIST, tenant_id, to_cstring(unit.server_));
        }
      }
      LOG_INFO("call notify resource to server", K(ret), "dst", unit.server_,
               "cost", ObTimeUtility::current_time() - start);
    }
  }
  return ret;
}

int ObUnitManager::get_tenant_unit_servers(
    const uint64_t tenant_id,
    const common::ObZone &zone,
    common::ObIArray<common::ObAddr> &server_array) const
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *pools = nullptr;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else {
    server_array.reset();
    int tmp_ret = get_pools_by_tenant(tenant_id, pools);
    if (OB_ENTRY_NOT_EXIST == tmp_ret) {
      // pass, and return empty server array
    } else if (OB_SUCCESS != tmp_ret) {
      ret = tmp_ret;
      LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
    } else if (nullptr == pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools ptr is null, unexpected", K(ret), K(tenant_id));
    } else {
      common::ObArray<common::ObAddr> this_server_array;
      for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
        this_server_array.reset();
        const share::ObResourcePool *pool = pools->at(i);
        if (OB_UNLIKELY(nullptr == pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool ptr is null", K(ret), K(tenant_id));
        } else if (OB_FAIL(get_pool_servers(
                pool->resource_pool_id_, zone, this_server_array))) {
          LOG_WARN("fail to get pool server", K(ret),
                   "pool_id", pool->resource_pool_id_, K(zone));
        } else if (OB_FAIL(append(server_array, this_server_array))) {
          LOG_WARN("fail to append", K(ret));
        }
      }
    }
  }
  return ret;
}


// allocate unit on target zones for specified resource pool
//
// @param [in] client                     SQL client
// @param [in] pool                       resource pool which new allocated units belong to
// @param [in] zones                      target zones to allocate units
// @param [in] unit_group_id_array        unit group id array for new allocated units
// @param [in] new_allocate_pool          whether to allocate new pool
// @param [in] increase_delta_unit_num    unit number to be increased
// @param [in] module                     module name for print log
// @param [out] new_servers               allocated servers for new units
//
// @ret OB_ZONE_RESOURCE_NOT_ENOUGH   zone resource not enough to hold all units
// @ret OB_ZONE_SERVER_NOT_ENOUGH     zone server not enough to hold all units
// @ret OB_SUCCESS                    success
int ObUnitManager::allocate_pool_units_(
    ObISQLClient &client,
    const share::ObResourcePool &pool,
    const common::ObIArray<common::ObZone> &zones,
    common::ObIArray<uint64_t> *unit_group_id_array,
    const bool new_allocate_pool,
    const int64_t increase_delta_unit_num,
    const char *module,
    ObIArray<ObAddr> &new_servers)
{
  int ret = OB_SUCCESS;
  ObUnitConfig *config = NULL;
  lib::Worker::CompatMode compat_mode = lib::Worker::CompatMode::INVALID;

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!pool.is_valid() || zones.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid resource pool or zones", K(pool), K(zones), KR(ret));
  } else if (increase_delta_unit_num <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(increase_delta_unit_num), K(ret));
  } else if (nullptr != unit_group_id_array
      && unit_group_id_array->count() != increase_delta_unit_num) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("new unit group id array status not match",
             KR(ret), K(increase_delta_unit_num), KP(unit_group_id_array));
  } else if (OB_UNLIKELY(nullptr == srv_rpc_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("srv_rpc_proxy_ ptr is null", K(ret));
  } else if (is_valid_tenant_id(pool.tenant_id_)
      && OB_FAIL(ObCompatModeGetter::get_tenant_mode(pool.tenant_id_, compat_mode))) {
    LOG_WARN("fail to get tenant compat mode", KR(ret), K(pool.tenant_id_));
  } else if (OB_FAIL(get_unit_config_by_id(pool.unit_config_id_, config))) {
    LOG_WARN("get_unit_config_by_id failed", "unit_config_id", pool.unit_config_id_, KR(ret));
  } else if (OB_ISNULL(config)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("config is null", KP(config), K(ret));
  } else {
    ObNotifyTenantServerResourceProxy notify_proxy(
        *srv_rpc_proxy_,
        &obrpc::ObSrvRpcProxy::notify_tenant_server_unit_resource);

    ObArray<ObAddr> excluded_servers;
    for (int64_t i = 0; OB_SUCC(ret) && i < zones.count(); ++i) { // for each zone
      const ObZone &zone = zones.at(i);
      excluded_servers.reuse();

      if (FAILEDx(get_excluded_servers(pool.resource_pool_id_, zone, module,
              new_allocate_pool, excluded_servers))) {
        LOG_WARN("get excluded servers fail", KR(ret), K(pool.resource_pool_id_), K(zone),
            K(module), K(new_allocate_pool));
      }

      for (int64_t j = 0; OB_SUCC(ret) && j < increase_delta_unit_num; ++j) {
        uint64_t unit_id = OB_INVALID_ID;
        std::string resource_not_enough_reason;
        ObAddr server;
        if (OB_FAIL(choose_server_for_unit(config->unit_resource(), zone, excluded_servers, module, server,
            resource_not_enough_reason))) {
          LOG_WARN("choose server for unit failed", K(module), KR(ret), "unit_idx", j, K(increase_delta_unit_num),
              K(zone), K(excluded_servers), KPC(config));
          // handle return error info
          if (OB_ZONE_RESOURCE_NOT_ENOUGH == ret) {
            LOG_USER_ERROR(OB_ZONE_RESOURCE_NOT_ENOUGH,
                to_cstring(zone), increase_delta_unit_num, resource_not_enough_reason.c_str());
          } else if (OB_ZONE_SERVER_NOT_ENOUGH == ret) {
            LOG_USER_ERROR(OB_ZONE_SERVER_NOT_ENOUGH, to_cstring(zone), increase_delta_unit_num);
          }
        } else if (OB_FAIL(excluded_servers.push_back(server))) {
          LOG_WARN("push_back failed", K(ret));
        } else if (OB_FAIL(fetch_new_unit_id(unit_id))) {
          LOG_WARN("fetch_new_unit_id failed", K(ret));
        } else {
          const bool is_delete = false; // is_delete is false when allocate new unit
          ObUnit unit;
          unit.reset();
          unit.unit_id_ = unit_id;
          unit.resource_pool_id_ = pool.resource_pool_id_;
          unit.unit_group_id_ = (nullptr == unit_group_id_array) ? 0 : unit_group_id_array->at(j);
          unit.server_ = server;
          unit.status_ = ObUnit::UNIT_STATUS_ACTIVE;
          unit.replica_type_ = pool.replica_type_;
          if (OB_FAIL(unit.zone_.assign(zone))) {
            LOG_WARN("fail to assign zone", KR(ret), K(zone));
          } else if (OB_FAIL(try_notify_tenant_server_unit_resource(
              pool.tenant_id_, is_delete, notify_proxy,
              pool, compat_mode, unit, false/*if not grant*/,
              false/*skip offline server*/))) {
            LOG_WARN("fail to try notify server unit resource", K(ret));
          } else if (OB_FAIL(add_unit(client, unit))) {
            LOG_WARN("add_unit failed", K(unit), K(ret));
          } else if (OB_FAIL(new_servers.push_back(server))) {
            LOG_WARN("push_back failed", K(ret));
          }
        }
      }
    }
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = notify_proxy.wait())) {
      LOG_WARN("fail to wait notify resource", K(ret), K(tmp_ret));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

int ObUnitManager::get_excluded_servers(const ObUnit &unit,
                                        const ObUnitStat &unit_stat,
                                        const char *module,
                                        ObIArray<ObAddr> &servers) const
{
  int ret = OB_SUCCESS;
  //Add all OBS whose disks do not meet the requirements
  ObArray<share::ObServerStatus> server_list;
  const bool new_allocate_pool = false;
  if (OB_FAIL(get_excluded_servers(unit.resource_pool_id_, unit.zone_, module,
          new_allocate_pool, servers))) {
    LOG_WARN("fail to get excluded_servers", K(ret), K(unit), K(new_allocate_pool));
  } else if (OB_FAIL(server_mgr_.get_server_statuses(unit.zone_, server_list))) {
    LOG_WARN("fail to get server of zone", K(ret), K(unit));
  } else {
    for (int64_t i = 0; i < server_list.count() && OB_SUCC(ret); i++) {
      ObServerStatus &status = server_list.at(i);
      bool is_exclude = false;
      if (!status.can_migrate_in()) {
        is_exclude = true;
        LOG_INFO("server can't migrate in, push into excluded_array", K(status), K(module));
      } else {
        int64_t required_size = unit_stat.required_size_ + status.resource_info_.disk_in_use_;
        int64_t total_size = status.resource_info_.disk_total_;
        if (total_size <= required_size || total_size <= 0) {
          is_exclude = true;
          LOG_INFO("server total size no bigger than required size", K(module), K(required_size), K(total_size),
                   K(unit_stat), K(status.resource_info_));
        } else if (required_size <= 0) {
          //nothing todo
        } else {
          int64_t required_percent = (100 * required_size) / total_size;
          int64_t limit_percent = GCONF.data_disk_usage_limit_percentage;
          if (required_percent > limit_percent) {
            is_exclude = true;
            LOG_INFO("server disk percent will out of control;", K(module), K(required_percent), K(limit_percent),
                     K(required_size), K(total_size));
          }
        }
      }
      if (!is_exclude) {
        //nothing todo
      } else if (has_exist_in_array(servers, status.server_)) {
        //nothing todo
      } else if (OB_FAIL(servers.push_back(status.server_))) {
        LOG_WARN("fail to push back", K(ret), K(status));
      }
    }
  }
  return ret;
}

// get excluded servers for resource pool on target zone.
//
// 1. resource pool units servers on target zone
// 2. tenant all resource pool units servers on target zone
//
// FIXME: 4.0 not support resource pool intersect in zone, here need not consider multiple pools of tenant
//
// @param [in] resource_pool_id   specified resource pool id
// @param [in] zone               specified zone which may be empty that means all zones
// @param [in] new_allocate_pool  new allocate pool 
// @param [out] servers           returned excluded servers
int ObUnitManager::get_excluded_servers(const uint64_t resource_pool_id,
    const ObZone &zone,
    const char *module,
    const bool new_allocate_pool, 
    ObIArray<ObAddr> &excluded_servers) const
{
  int ret = OB_SUCCESS;
  share::ObResourcePool *pool = NULL;
  int64_t tenant_id = OB_INVALID_ID;
  common::ObArray<common::ObAddr> sys_standalone_servers;
  common::ObArray<share::ObResourcePool *> *all_pools = NULL;

  excluded_servers.reset();
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(OB_INVALID_ID == resource_pool_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid resource pool id", KR(ret), K(resource_pool_id));
  } else if (new_allocate_pool) {
    //nothing todo
  } else if (OB_FAIL(get_resource_pool_by_id(resource_pool_id, pool))) {
    LOG_WARN("fail to get resource pool by id", K(ret), K(resource_pool_id));
  } else if (FALSE_IT(tenant_id = pool->tenant_id_)) {
    //nothing todo
  } else if (! is_valid_tenant_id(tenant_id)) {
    // The pool does not belong to any tenant, only get resource pool servers
    if (OB_FAIL(get_pool_servers(resource_pool_id, zone, excluded_servers))) {
      LOG_WARN("get_pool_servers failed", "resource pool id",
          resource_pool_id, K(zone), KR(ret));
    }
  }
  // get all tenant resource pool related servers on target zone
  else if (OB_FAIL(get_tenant_unit_servers(tenant_id, zone, excluded_servers))) {
    LOG_WARN("get tennat unit server fail", KR(ret), K(tenant_id), K(zone));
  }

  if (OB_SUCC(ret) && GCONF.enable_sys_unit_standalone) {
    // When the system tenant is deployed independently,
    // the server where the unit of the system tenant is located is also required as the executed servers
    if (OB_FAIL(get_tenant_unit_servers(OB_SYS_TENANT_ID, zone, sys_standalone_servers))) {
      LOG_WARN("fail to get tenant unit servers", KR(ret), K(zone));
    } else if (OB_FAIL(append(excluded_servers, sys_standalone_servers))) {
      LOG_WARN("fail to append other excluded servers", K(ret));
    }
  }

  LOG_INFO("get tenant resource pool servers and sys standalone servers as excluded servers",
      K(module), K(new_allocate_pool),
      KR(ret),
      K(resource_pool_id),
      K(tenant_id),
      K(zone),
      "enable_sys_unit_standalone", GCONF.enable_sys_unit_standalone,
      K(excluded_servers),
      K(sys_standalone_servers));
  return ret;
}


int ObUnitManager::get_pools_servers(const common::ObIArray<share::ObResourcePool  *> &pools,
    common::hash::ObHashMap<common::ObAddr, int64_t> &server_ref_count_map) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (pools.count() <= 0 || !server_ref_count_map.created()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pools is empty", K(pools),
        "server_ref_count_map created", server_ref_count_map.created(), K(ret));
  } else {
    ObArray<ObAddr> servers;
    const ObZone all_zone;
    FOREACH_CNT_X(pool, pools, OB_SUCCESS == ret) {
      servers.reuse();
      if (NULL == *pool) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("pool is null", "pool", *pool, K(ret));
      } else if (OB_FAIL(get_pool_servers((*pool)->resource_pool_id_, all_zone, servers))) {
        LOG_WARN("get pool servers failed",
            "pool id", (*pool)->resource_pool_id_, K(all_zone), K(ret));
      } else {
        FOREACH_CNT_X(server, servers, OB_SUCCESS == ret) {
          int64_t server_ref_count = 0;
          if (OB_FAIL(get_server_ref_count(server_ref_count_map, *server, server_ref_count))) {
            if (OB_ENTRY_NOT_EXIST == ret) {
              server_ref_count = 1;
              ret = OB_SUCCESS;
            } else {
              LOG_WARN("get server ref count failed", "server", *server, K(ret));
            }
          } else {
            ++server_ref_count;
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(set_server_ref_count(server_ref_count_map, *server, server_ref_count))) {
              LOG_WARN("set server ref count failed",
                  "server", *server, K(server_ref_count), K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_pool_servers(const uint64_t resource_pool_id,
                                    const ObZone &zone,
                                    ObIArray<ObAddr> &servers) const
{
  int ret = OB_SUCCESS;
  ObArray<ObUnit *> *units = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    // don't need to check zone, can be empty
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid resource_pool_id", K(resource_pool_id), K(ret));
  } else if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
    LOG_WARN("get_units_by_pool failed", K(resource_pool_id), K(ret));
  } else if (NULL == units) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units is null", KP(units), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      if (NULL == units->at(i)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit is null", "unit", OB_P(units->at(i)), K(ret));
      } else if (!zone.is_empty() && zone != units->at(i)->zone_) {
        continue;
      } else {
        if (OB_SUCCESS == ret && units->at(i)->migrate_from_server_.is_valid()) {
          if (OB_FAIL(servers.push_back(units->at(i)->migrate_from_server_))) {
            LOG_WARN("push_back failed", K(ret));
          }
        }

        if (OB_SUCCESS == ret && units->at(i)->server_.is_valid()) {
          if (OB_FAIL(servers.push_back(units->at(i)->server_))) {
            LOG_WARN("push_back failed", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

// choose server for target one unit
//
// @param [in] config                         target unit resource
// @param [in] zone                           target zone
// @param [in] excluded_servers               excluded servers which can not allocate unit
// @param [in] module                         module name for log print
// @param [out] server                        choosed server for unit
// @param [out] resource_not_enough_reason    reason for resource not enough,
//                                            is valid when ret = OB_ZONE_RESOURCE_NOT_ENOUGH
//
// @ret OB_SUCCESS                    on success
// @ret OB_ZONE_RESOURCE_NOT_ENOUGH   zone resource not enough to hold new unit
// @ret OB_ZONE_SERVER_NOT_ENOUGH     all valid servers are excluded, no server to hold new unit
int ObUnitManager::choose_server_for_unit(const ObUnitResource &config,
                                          const ObZone &zone,
                                          const ObArray<ObAddr> &excluded_servers,
                                          const char *module,
                                          ObAddr &choosed_server,
                                          std::string &resource_not_enough_reason) const
{
  int ret = OB_SUCCESS;
  ObArray<ObServerStatus> server_statuses;
  ObArray<ObUnitPlacementStrategy::ObServerResource> server_resources;

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!config.is_valid() || zone.is_empty()) {
    // excluded_servers can be empty
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid config", K(config), K(zone), K(ret));
  } else if (OB_FAIL(server_mgr_.get_server_statuses(zone, server_statuses))) {
    LOG_WARN("get_server_statuses failed", K(zone), K(ret));
  } else if (OB_FAIL(build_server_resources_(server_statuses, server_resources))) {
    LOG_WARN("build server resources fail", KR(ret), K(server_statuses));
  } else if (OB_FAIL(do_choose_server_for_unit_(config, zone, excluded_servers, server_statuses,
      server_resources, module, choosed_server, resource_not_enough_reason))) {
    LOG_WARN("fail to choose server for unit", K(module), KR(ret), K(config), K(zone), K(excluded_servers),
        K(server_statuses), K(server_resources),
        "resource_not_enough_reason", resource_not_enough_reason.c_str());
  }
  return ret;
}


int ObUnitManager::do_choose_server_for_unit_(const ObUnitResource &config,
                                              const ObZone &zone,
                                              const ObArray<ObAddr> &excluded_servers,
                                              const ObIArray<share::ObServerStatus> &statuses,
                                              const ObIArray<ObUnitPlacementStrategy::ObServerResource> &server_resources,
                                              const char *module,
                                              ObAddr &choosed_server,
                                              std::string &resource_not_enough_reason) const
{
  LOG_INFO("[CHOOSE_SERVER_FOR_UNIT] begin", K(module), K(zone), K(excluded_servers), K(config));

  int ret = OB_SUCCESS;
  double hard_limit = 1.0;
  ObArray<ObUnitPlacementStrategy::ObServerResource> valid_server_resources;

  choosed_server.reset();

  if (OB_UNLIKELY(zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("zone is empty, unexpected", KR(ret), K(zone));
  } else if (statuses.count() != server_resources.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server statuses and server resources array", KR(ret), K(statuses), K(server_resources));
  } else if (OB_FAIL(get_hard_limit(hard_limit))) {
    LOG_WARN("get_hard_limit failed", K(ret));
  } else {
    int64_t not_excluded_server_count = 0;
    // 1. construct valid servers resource
    for (int64_t i = 0; OB_SUCC(ret) && i < statuses.count(); ++i) { // for each active servers
      ObResourceType not_enough_resource = RES_MAX;
      AlterResourceErr not_enough_resource_config = ALT_ERR;
      const ObServerStatus &server_status = statuses.at(i);
      const ObUnitPlacementStrategy::ObServerResource &server_resource = server_resources.at(i);

      if (has_exist_in_array(excluded_servers, server_status.server_)) {
        // excluded servers are expected, need not show in reason
        continue;
      } else {
        not_excluded_server_count++;

        if (!server_status.can_migrate_in()) {
          if (! server_status.is_active()) {
            resource_not_enough_reason =
                resource_not_enough_reason + "server '" + to_cstring(server_status.server_) + "' is not active\n";
          } else {
            // server is block-migrate-in
            resource_not_enough_reason =
                resource_not_enough_reason + "server '" + to_cstring(server_status.server_) + "' is blocked migrate-in\n";
          }
          LOG_WARN("[CHOOSE_SERVER_FOR_UNIT] server can not migrate in", K(module), K(i), "server", server_status);
          continue;
        } else {
          bool is_resource_enough =
              check_resource_enough_for_unit_(server_resource, config, hard_limit,
                  not_enough_resource, not_enough_resource_config);

          if (is_resource_enough) {
            LOG_INFO("[CHOOSE_SERVER_FOR_UNIT] find available server", K(module), K(i),
                K(server_resource), "request_unit_config", config);

            if (OB_FAIL(valid_server_resources.push_back(server_resource))) {
              LOG_WARN("failed to push into array", KR(ret), K(server_resource));
            }
          } else {
            LOG_INFO("[CHOOSE_SERVER_FOR_UNIT] server resource not enough", K(module), K(i),
                "not_enough_resource", resource_type_to_str(not_enough_resource),
                "not_enough_resource_config", alter_resource_err_to_str(not_enough_resource_config),
                K(server_resource), "request_unit_config", config);
            resource_not_enough_reason =
                resource_not_enough_reason + "server '" + to_cstring(server_status.server_) + "' "
                + resource_type_to_str(not_enough_resource) + " resource not enough\n";
          }
        }
      }
    }  // end for

    if (OB_SUCCESS == ret) {
      if (0 == not_excluded_server_count) {
        ret = OB_ZONE_SERVER_NOT_ENOUGH;
        LOG_WARN("zone server not enough to hold all units", K(module), KR(ret), K(zone), K(excluded_servers),
            K(statuses));
      } else if (valid_server_resources.count() <= 0) {
        ret = OB_ZONE_RESOURCE_NOT_ENOUGH;
        LOG_WARN("zone resource is not enough to hold a new unit", K(module), KR(ret), K(zone),
            K(config), K(excluded_servers),
            "resource_not_enough_reason", resource_not_enough_reason.c_str());
      } else {
        // 2. choose the server
        ObUnitPlacementDPStrategy unit_placement;
        if (OB_FAIL(unit_placement.choose_server(valid_server_resources, config, module, choosed_server))) {
          LOG_WARN("failed to choose server for unit", K(ret), K(config));
        }
      }
    }
  }
  LOG_INFO("[CHOOSE_SERVER_FOR_UNIT] end", K(module), KR(ret), K(choosed_server), K(zone), K(excluded_servers),
      K(config), K(valid_server_resources),
      "resource_not_enough_reason", resource_not_enough_reason.c_str());
  return ret;
}

int ObUnitManager::compute_server_resource_(const ObServerStatus &server_status,
    ObUnitPlacementStrategy::ObServerResource &server_resource) const
{
  int ret = OB_SUCCESS;
  ObUnitConfig sum_load;
  ObArray<ObUnitLoad> *unit_loads = NULL;

  if (OB_UNLIKELY(!server_status.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(server_status), KR(ret));
  } else if (OB_FAIL(get_loads_by_server(server_status.server_, unit_loads))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_loads_by_server failed", "server", server_status.server_, K(ret));
    } else {
      ret = OB_SUCCESS;
    }
  } else if (NULL == unit_loads) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unit_loads is null", KP(unit_loads), K(ret));
  } else if (OB_FAIL(calc_sum_load(unit_loads, sum_load))) {
    LOG_WARN("calc_sum_load failed", KP(unit_loads), K(ret));
  }

  if (OB_SUCC(ret)) {
    // Unit resource information is persisted on the observer side,
    // The unit_load seen on the rs side is only the resource view managed by rs itself,
    // It is actually inaccurate to judge whether the resources are sufficient based on the resource view of rs itself,
    // Need to consider the persistent resources of the unit resource information on the observer side.
    // The persistent information of the unit on the observer side is regularly reported to rs by the observer through the heartbeat.
    // When performing allocation, rs reports the maximum value of resource information from its own resource view
    // and observer side as a reference for unit resource allocation

    const ObServerResourceInfo &report_resource = server_status.resource_info_;
    server_resource.addr_ = server_status.server_;
    server_resource.assigned_[RES_CPU] = sum_load.min_cpu() > report_resource.report_cpu_assigned_
                                         ? sum_load.min_cpu() : report_resource.report_cpu_assigned_;
    server_resource.max_assigned_[RES_CPU] = sum_load.max_cpu() > report_resource.report_cpu_max_assigned_
                                         ? sum_load.max_cpu() : report_resource.report_cpu_max_assigned_;
    server_resource.capacity_[RES_CPU] = server_status.resource_info_.cpu_;
    server_resource.assigned_[RES_MEM] = sum_load.memory_size() > report_resource.report_mem_assigned_
                                         ? static_cast<double>(sum_load.memory_size())
                                         : static_cast<double>(report_resource.report_mem_assigned_);
    server_resource.max_assigned_[RES_MEM] = server_resource.assigned_[RES_MEM];
    server_resource.capacity_[RES_MEM] = static_cast<double>(server_status.resource_info_.mem_total_);
    server_resource.assigned_[RES_LOG_DISK] = static_cast<double>(sum_load.log_disk_size());
    server_resource.max_assigned_[RES_LOG_DISK] = static_cast<double>(sum_load.log_disk_size());
    server_resource.capacity_[RES_LOG_DISK] = static_cast<double>(server_status.resource_info_.log_disk_total_);
  }

  LOG_INFO("compute server resource", KR(ret),
            "server", server_status.server_,
            K(server_resource),
            "report_resource_info", server_status.resource_info_,
            "valid_unit_sum", sum_load,
            "valid_unit_count", unit_loads != NULL ? unit_loads->count(): 0);
  return ret;
}

// check resource enough for unit
//
// @param [in] u                              demands resource that may have some invalid items, need not check valid
// @param [out] not_enough_resource           returned resource type that is not enough
// @param [out] not_enough_resource_config    returned resource config type that is not enough
bool ObUnitManager::check_resource_enough_for_unit_(
    const ObUnitPlacementStrategy::ObServerResource &r,
    const ObUnitResource &u,
    const double hard_limit,
    ObResourceType &not_enough_resource,
    AlterResourceErr &not_enough_resource_config) const
{
  bool is_enough = false; // default is false

  if (u.is_max_cpu_valid() &&
      r.capacity_[RES_CPU] * hard_limit < r.max_assigned_[RES_CPU] + u.max_cpu()) {
    not_enough_resource = RES_CPU;
    not_enough_resource_config = MAX_CPU;
  } else if (u.is_min_cpu_valid() &&
             r.capacity_[RES_CPU] < r.assigned_[RES_CPU] + u.min_cpu()) {
    not_enough_resource = RES_CPU;
    not_enough_resource_config = MIN_CPU;
  } else if (u.is_memory_size_valid() &&
             r.capacity_[RES_MEM] < r.assigned_[RES_MEM] + u.memory_size()) {
    not_enough_resource = RES_MEM;
    not_enough_resource_config = MEMORY;
  } else if (u.is_log_disk_size_valid() &&
             r.capacity_[RES_LOG_DISK] < r.assigned_[RES_LOG_DISK] + u.log_disk_size()) {
    not_enough_resource = RES_LOG_DISK;
    not_enough_resource_config = LOG_DISK;
  } else {
    is_enough = true;
    not_enough_resource = RES_MAX;
    not_enough_resource_config = ALT_ERR;
  }

  if (! is_enough) {
    _LOG_INFO("server %s resource '%s' is not enough for unit. hard_limit=%.6g, server_resource=%s, "
        "demands=%s",
        resource_type_to_str(not_enough_resource),
        alter_resource_err_to_str(not_enough_resource_config),
        hard_limit,
        to_cstring(r),
        to_cstring(u));
  }
  return is_enough;
}


// demand_resource may have some invalid items, need not check valid for demand_resource
int ObUnitManager::have_enough_resource(const ObServerStatus &server_status,
                                        const ObUnitResource &demand_resource,
                                        const double hard_limit,
                                        bool &is_enough,
                                        AlterResourceErr &err_index) const
{
  int ret = OB_SUCCESS;
  ObResourceType not_enough_resource = RES_MAX;
  ObUnitPlacementStrategy::ObServerResource server_resource;

  err_index = ALT_ERR;

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!server_status.is_valid() || hard_limit <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(server_status), K(hard_limit), K(ret));
  } else if (OB_FAIL(compute_server_resource_(server_status, server_resource))) {
    LOG_WARN("compute server resource fail", KR(ret), K(server_status));
  } else {
    is_enough = check_resource_enough_for_unit_(server_resource, demand_resource, hard_limit,
        not_enough_resource, err_index);
  }
  return ret;
}

int ObUnitManager::check_enough_resource_for_delete_server(
    const ObAddr &server,
    const ObZone &zone)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnitManager::ObUnitLoad> *unit_loads = NULL;
  ObArray<ObUnitPlacementStrategy::ObServerResource> initial_servers_resources;
  ObArray<ObServerStatus> statuses;
  SpinRLockGuard guard(lock_);
  bool empty = false;
  if (OB_UNLIKELY(zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("zone is invalid", KR(ret), K(zone), K(server));
  } else if (OB_FAIL(check_server_empty(server, empty))) {
    LOG_WARN("fail to check server empty", K(ret));
  } else if (empty) {
    //nothing todo
  } else {
    if (OB_FAIL(get_loads_by_server(server, unit_loads))) {
      LOG_WARN("fail to get loads by server", K(ret));
    } else if (OB_FAIL(server_mgr_.get_server_statuses(zone, statuses))) {
      LOG_WARN("get_server_statuses failed", K(zone), K(ret));
    } else if (OB_FAIL(build_server_resources_(statuses, initial_servers_resources))) {
      LOG_WARN("fail to build server resources", KR(ret), K(statuses));
    } else {
      for (int64_t i = 0; i < unit_loads->count() && OB_SUCC(ret); ++i) {
        std::string resource_not_enough_reason;
        if (OB_FAIL(check_server_have_enough_resource_for_delete_server_(
                    unit_loads->at(i),
                    zone,
                    statuses,
                    initial_servers_resources,
                    resource_not_enough_reason))) {
          LOG_WARN("fail to check server have enough resource for delete server",
              K(ret),
              K(zone),
              K(initial_servers_resources),
              K(i),
              K(unit_loads),
              K(resource_not_enough_reason.c_str()));

          // handle return error info
          if (OB_ZONE_SERVER_NOT_ENOUGH == ret) {
            std::string err_msg;
            const ObUnit *unit = unit_loads->at(i).unit_;
            uint64_t unit_id = (NULL == unit ? 0 : unit->unit_id_);

            err_msg = err_msg + "can not migrate out unit '" + to_cstring(unit_id) +
                "', no other available servers on zone '" +  to_cstring(zone) +
                "', delete server not allowed";
            LOG_USER_ERROR(OB_DELETE_SERVER_NOT_ALLOWED, err_msg.c_str());
          } else if (OB_ZONE_RESOURCE_NOT_ENOUGH == ret) {
            std::string err_msg;
            const ObUnit *unit = unit_loads->at(i).unit_;
            uint64_t unit_id = (NULL == unit ? 0 : unit->unit_id_);

            err_msg = err_msg + "can not migrate out all units, zone '" + to_cstring(zone) +
                "' resource not enough, delete server not allowed. "
                "You can check resource info by views: DBA_OB_UNITS, GV$OB_UNITS, GV$OB_SERVERS.\n"
                + resource_not_enough_reason.c_str();
            LOG_USER_ERROR(OB_DELETE_SERVER_NOT_ALLOWED, err_msg.c_str());
          }
        }
      } //end for unit_loads
    }
  }
  return ret;
}

int ObUnitManager::check_server_have_enough_resource_for_delete_server_(
    const ObUnitLoad &unit_load,
    const common::ObZone &zone,
    const ObIArray<ObServerStatus> &statuses,
    ObIArray<ObUnitPlacementStrategy::ObServerResource> &initial_servers_resources,
    std::string &resource_not_enough_reason)
{
  int ret = OB_SUCCESS;
  double hard_limit = 1.0;
  ObArray<ObUnitPlacementStrategy::ObServerResource> servers_resources;
  ObArray<ObAddr> excluded_servers;
  ObAddr choosed_server;
  const ObUnitConfig *config = unit_load.unit_config_;
  const char *module = "DELETE_SERVER";
  const bool new_allocate_pool = false;

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_ISNULL(config) || OB_UNLIKELY(! config->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit config ptr is null", KR(ret), KPC(config));
  } else if (OB_UNLIKELY(zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("zone is empty, unexpected", KR(ret), K(zone));
  } else if (statuses.count() != initial_servers_resources.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("server statuses does not match initial_servers_resources", KR(ret), K(statuses),
        K(initial_servers_resources));
  } else if (OB_ISNULL(unit_load.unit_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unit ptr is null", K(ret));
  } else if (unit_load.unit_->migrate_from_server_.is_valid()) {
    // In a state of migration, there must be a place to migrate
  } else if (OB_FAIL(get_excluded_servers(unit_load.unit_->resource_pool_id_, unit_load.unit_->zone_, module,
          new_allocate_pool, excluded_servers))) {
    LOG_WARN("fail to get excluded server", K(module), KR(ret), KPC(unit_load.unit_), K(new_allocate_pool));
  }
  // choose right server for target unit
  else if (OB_FAIL(do_choose_server_for_unit_(config->unit_resource(), zone,
      excluded_servers, statuses, initial_servers_resources,
      module, choosed_server, resource_not_enough_reason))) {
    LOG_WARN("choose server for unit fail", K(module), KR(ret), K(zone), KPC(config),
        K(excluded_servers), K(statuses), K(initial_servers_resources),
        "resource_not_enough_reason", resource_not_enough_reason.c_str());
  } else {
    // sum target unit resource config on choosed server resource
    int64_t choosed_index = -1;
    for (int64_t i = 0; choosed_index < 0 && i < initial_servers_resources.count(); i++) {
      if (initial_servers_resources.at(i).addr_ == choosed_server) {
        choosed_index = i;
      }
    }

    if (OB_UNLIKELY(choosed_index < 0 || choosed_index >= initial_servers_resources.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("can not find choosed_server in initial_servers_resources, or invalid choosed_index", KR(ret),
          K(choosed_index), K(choosed_server), K(initial_servers_resources));
    } else if (OB_FAIL(sum_servers_resources(initial_servers_resources.at(choosed_index), *config))) {
      LOG_WARN("fail to sum servers resources", KR(ret), K(initial_servers_resources),
          KPC(config));
    }
  }
  return ret;
}

int ObUnitManager::build_server_resources_(
                   const ObIArray<ObServerStatus> &statuses,
                   ObIArray<ObUnitPlacementStrategy::ObServerResource> &servers_resources) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < statuses.count(); ++i) {
    const ObServerStatus &server_status = statuses.at(i);
    ObUnitPlacementStrategy::ObServerResource server_resource;

    if (OB_FAIL(compute_server_resource_(server_status, server_resource))) {
      LOG_WARN("compute server resource fail", KR(ret), K(server_status));
    } else if (OB_FAIL(servers_resources.push_back(server_resource))) {
      LOG_WARN("fail to push back", KR(ret), K(server_resource));
    }
  }
  return ret;
}

int ObUnitManager::sum_servers_resources(ObUnitPlacementStrategy::ObServerResource &server_resource,
                                         const share::ObUnitConfig &unit_config)
{
  int ret = OB_SUCCESS;
  server_resource.assigned_[RES_CPU] = server_resource.assigned_[RES_CPU] +
                                       unit_config.min_cpu();
  server_resource.max_assigned_[RES_CPU] = server_resource.max_assigned_[RES_CPU] +
                                           unit_config.max_cpu();
  server_resource.assigned_[RES_MEM] = server_resource.assigned_[RES_MEM] +
                                       static_cast<double>(unit_config.memory_size());
  server_resource.max_assigned_[RES_MEM] = server_resource.max_assigned_[RES_MEM] +
                                           static_cast<double>(unit_config.memory_size());
  server_resource.assigned_[RES_LOG_DISK] = server_resource.assigned_[RES_LOG_DISK] +
                                        static_cast<double>(unit_config.log_disk_size());
  server_resource.max_assigned_[RES_LOG_DISK] = server_resource.max_assigned_[RES_LOG_DISK] +
                                            static_cast<double>(unit_config.log_disk_size());
  return ret;
}

int ObUnitManager::add_unit(ObISQLClient &client, const ObUnit &unit)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!unit.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(unit), K(ret));
  } else {
    ObUnit *new_unit = NULL;
    if (OB_SUCCESS == (ret = get_unit_by_id(unit.unit_id_, new_unit))) {
      ret = OB_ENTRY_EXIST;
      LOG_WARN("unit already exist, can't add", K(unit), K(ret));
    } else if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_unit_by_id failed", "unit_id", unit.unit_id_, K(ret));
    } else {
      ret = OB_SUCCESS;
    }

    if (OB_FAIL(ret)) {
    } else if (NULL == (new_unit = allocator_.alloc())) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("alloc unit failed", K(ret));
    } else {
      if (OB_FAIL(ut_operator_.update_unit(client, unit))) {
        LOG_WARN("update_unit failed", K(unit), K(ret));
      } else {
        *new_unit = unit;
        if (OB_FAIL(insert_unit(new_unit))) {
          LOG_WARN("insert_unit failed", "new unit", *new_unit, K(ret));
        } else {
          ROOTSERVICE_EVENT_ADD("unit", "create_unit",
              "unit_id", unit.unit_id_,
              "server", unit.server_);
        }
      }
      if (OB_FAIL(ret)) {
        //avoid memory leak
        allocator_.free(new_unit);
        new_unit = NULL;
      }
    }
  }
  return ret;
}

int ObUnitManager::alter_pool_unit_config(share::ObResourcePool  *pool,
                                          const ObUnitConfigName &config_name)
{
  int ret = OB_SUCCESS;
  ObUnitConfig *config = NULL;
  ObUnitConfig *alter_config = NULL;
  common::ObSEArray<share::ObResourcePool *, 1> pools;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is null", K(ret));
  } else if (config_name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("config_name is empty", K(config_name), K(ret));
  } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, config))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_unit_config_by_id failed", "unit_config_id", pool->unit_config_id_, K(ret));
    } else {
      ret = OB_RESOURCE_UNIT_NOT_EXIST;
      LOG_WARN("unit config not exist", "config_id", pool->unit_config_id_, K(ret));
    }
  } else if (NULL == config) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("config is null", KP(config), K(ret));
  } else if (OB_FAIL(get_unit_config_by_name(config_name, alter_config))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_unit_config_by_name failed", K(config_name), K(ret));
    } else {
      ret = OB_RESOURCE_UNIT_NOT_EXIST;
      LOG_USER_ERROR(OB_RESOURCE_UNIT_NOT_EXIST, to_cstring(config_name));
      LOG_WARN("unit config not exist", K(config_name), K(ret));
    }
  } else if (NULL == alter_config) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("alter_config is null", KP(alter_config), K(ret));
  } else if (config_name == config->name()) {
    // do nothing
  } else if (REPLICA_TYPE_FULL == pool->replica_type_
      && alter_config->unit_resource().memory_size() < GCONF.__min_full_resource_pool_memory) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("full resource pool memory size illegal", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "unit MEMORY_SIZE less than __min_full_resource_pool_memory");
  } else if (OB_FAIL(pools.push_back(pool))) {
    LOG_WARN("push back pool into array fail", KR(ret), K(pool), K(pools));
  } else if (OB_FAIL(check_expand_resource_(
      "ALTER_RESOURCE_POOL_UNIT_CONFIG",
      pools,
      config->unit_resource(),
      alter_config->unit_resource()))) {
    LOG_WARN("check_expand_config failed", KR(ret), KPC(pool), KPC(config), KPC(alter_config));
  } else if (OB_FAIL(check_shrink_resource_(*pool, config->unit_resource(),
      alter_config->unit_resource()))) {
    LOG_WARN("check_shrink_resource_ failed", KPC(pool), KPC(config), KPC(alter_config), KR(ret));
  } else if (OB_FAIL(change_pool_config(pool, config, alter_config))) {
    LOG_WARN("change_pool_config failed", "pool", *pool, "config", *config,
        "alter_config", *alter_config, K(ret));
  }
  return ret;
}

// There are two situations to consider in the shrinkage of the resource pool:
// 1. There is no resource pool granted to tenants
//    the specified number of units can be directly deleted when the resource pool shrinks.
// 2. Resource pool that has been granted to tenants
//    It is necessary to select the specified units from the resource pool,
//    mark these units as the deleting state,
//    and wait for all the replicas to be moved from such units before the whole process can be considered complete.
//    The unit num of the pool that has been granted to the tenant needs to be scaled down.
//    Preconditions:
//    2.1 The reduced unit num cannot be less than the number of replicas of locality in the corresponding zone
//    2.2 At present, the shrinking operation is performed when the unit is in a steady state,
//        the shrinking is allowed when all units are not migrated.
//        We avoid the unit being in both the migration and the deleting state, thereby reducing the complexity
int ObUnitManager::shrink_pool_unit_num(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_id_array)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else {
    ObArray<uint64_t> sort_unit_id_array;
    for (int64_t i = 0; OB_SUCC(ret) && i < delete_unit_id_array.count(); ++i) {
      if (OB_FAIL(sort_unit_id_array.push_back(delete_unit_id_array.at(i)))) {
        LOG_WARN("fail to push back", K(ret));
      }
    }
    std::sort(sort_unit_id_array.begin(), sort_unit_id_array.end());
    for (int64_t i = 0; OB_SUCC(ret) && i < sort_unit_id_array.count() - 1; ++i) {
      if (sort_unit_id_array.at(i) == sort_unit_id_array.at(i + 1)) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("delete same unit more than once not supported", K(ret));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "delete same unit more than once");
      }
    }
    if (OB_SUCC(ret)) {
      uint64_t tenant_id = pool->tenant_id_;
      if (! is_valid_tenant_id(tenant_id)) {
        if (OB_FAIL(shrink_not_granted_pool(pool, alter_unit_num, delete_unit_id_array))) {
          LOG_WARN("fail to shrink not granted pool", K(ret));
        }
      } else {
        if (OB_FAIL(shrink_granted_pool(pool, alter_unit_num, delete_unit_id_array))) {
          LOG_WARN("fail to shrink granted pool", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::build_sorted_zone_unit_ptr_array(
    share::ObResourcePool *pool,
    common::ObIArray<ZoneUnitPtr> &zone_unit_ptrs)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *units = NULL;
  zone_unit_ptrs.reset();
  if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool));
  } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret), "pool_id", pool->resource_pool_id_);
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units ptr is null", K(ret), KP(units));
  } else {
    // traverse all unit in units
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      ObUnit *this_unit = units->at(i);
      if (OB_UNLIKELY(NULL == this_unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret), KP(this_unit));
      } else {
        // aggregate units with the same zone
        int64_t index = 0;
        for (; OB_SUCC(ret) && index < zone_unit_ptrs.count(); ++index) {
          if (this_unit->zone_ == zone_unit_ptrs.at(index).zone_) {
            break;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (index >= zone_unit_ptrs.count()) {
          ZoneUnitPtr zone_unit_ptr;
          zone_unit_ptr.zone_ = this_unit->zone_;
          if (OB_FAIL(zone_unit_ptrs.push_back(zone_unit_ptr))) {
            LOG_WARN("fail to push back", K(ret));
          }
        }
        if (OB_FAIL(ret)) {
        } else if (index >= zone_unit_ptrs.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected index", K(ret), K(index),
                   "zone_unit_count", zone_unit_ptrs.count());
        } else if (this_unit->zone_ != zone_unit_ptrs.at(index).zone_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("zone not match", K(ret), "left_zone", this_unit->zone_,
                   "right_zone", zone_unit_ptrs.at(index).zone_);
        } else if (OB_FAIL(zone_unit_ptrs.at(index).unit_ptrs_.push_back(this_unit))) {
          LOG_WARN("fail to push back", K(ret), K(index));
        } else {} // good, no more to do
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < zone_unit_ptrs.count(); ++i) {
      // sort each zone unit ptr using group id
      ZoneUnitPtr &zone_unit_ptr = zone_unit_ptrs.at(i);
      if (OB_FAIL(zone_unit_ptr.sort_by_unit_id_desc())) {
        LOG_WARN("fail to sort unit", K(ret));
      } else {} // good, unit num match
    }
  }
  return ret;
}

int ObUnitManager::check_shrink_unit_num_zone_condition(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_id_array)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (delete_unit_id_array.count() <= 0) {
    // good, we choose deleting set all by ourselves
  } else if (pool->unit_count_ <= alter_unit_num) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should be a shrink pool operation", K(ret),
             "cur_unit_num", pool->unit_count_,
             "new_unit_num", alter_unit_num);
  } else {
    int64_t delta = pool->unit_count_ - alter_unit_num;
    const common::ObIArray<common::ObZone> &zone_list = pool->zone_list_;
    common::ObArray<ZoneUnitPtr> delete_zone_unit_ptrs;
    for (int64_t i = 0; OB_SUCC(ret) && i < delete_unit_id_array.count(); ++i) {
      ObUnit *this_unit = NULL;
      if (OB_FAIL(get_unit_by_id(delete_unit_id_array.at(i), this_unit))) {
        LOG_WARN("fail to get unit by id", K(ret));
      } else if (OB_UNLIKELY(NULL == this_unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret), KP(this_unit));
      } else if (this_unit->resource_pool_id_ != pool->resource_pool_id_) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("do not support shrink unit belonging to other pool");
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "shrink unit belonging to other pool");
      } else if (!has_exist_in_array(zone_list, this_unit->zone_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit zone not match", K(ret), K(zone_list), "unit_zone", this_unit->zone_);
      } else {
        int64_t index = 0;
        for (; OB_SUCC(ret) && index < delete_zone_unit_ptrs.count(); ++index) {
          if (this_unit->zone_ == delete_zone_unit_ptrs.at(index).zone_) {
            break;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (index >= delete_zone_unit_ptrs.count()) {
          ZoneUnitPtr delete_zone_unit_ptr;
          delete_zone_unit_ptr.zone_ = this_unit->zone_;
          if (OB_FAIL(delete_zone_unit_ptrs.push_back(delete_zone_unit_ptr))) {
            LOG_WARN("fail to push back", K(ret));
          }
        }
        if (OB_FAIL(ret)) {
        } else if (index >= delete_zone_unit_ptrs.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected index", K(ret), K(index),
                   "delete_zone_unit_count", delete_zone_unit_ptrs.count());
        } else if (this_unit->zone_ != delete_zone_unit_ptrs.at(index).zone_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("zone not match", K(ret), "left_zone", this_unit->zone_,
                   "right_zone", delete_zone_unit_ptrs.at(index).zone_);
        } else if (OB_FAIL(delete_zone_unit_ptrs.at(index).unit_ptrs_.push_back(this_unit))) {
          LOG_WARN("fail to push back", K(ret), K(index));
        } else {} // good, no more to do
      }
    }
    if (OB_SUCC(ret)) {
      if (delete_zone_unit_ptrs.count() != zone_list.count()) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("do not support shrink unit num to different value on different zone", K(ret));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "shrink unit num to different value on different zone");
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < delete_zone_unit_ptrs.count(); ++i) {
        if (delta != delete_zone_unit_ptrs.at(i).unit_ptrs_.count()) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("shrink mismatching unit num and unit id list not support", K(ret));
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "shrink mismatching unit num and unit id list");
        } else {} // good, go on to check next
      }
    }
  }
  return ret;
}

int ObUnitManager::fill_delete_unit_ptr_array(
    share::ObResourcePool *pool,
    const common::ObIArray<uint64_t> &delete_unit_id_array,
    const int64_t alter_unit_num,
    common::ObIArray<ObUnit *> &output_delete_unit_ptr_array)
{
  int ret = OB_SUCCESS;
  output_delete_unit_ptr_array.reset();
  if (delete_unit_id_array.count() > 0) {
    // The alter resource pool shrinkage specifies the deleted unit, just fill it in directly
    for (int64_t i = 0; OB_SUCC(ret) && i < delete_unit_id_array.count(); ++i) {
      ObUnit *unit = NULL;
      if (OB_FAIL(get_unit_by_id(delete_unit_id_array.at(i), unit))) {
        LOG_WARN("fail to get unit by id", K(ret));
      } else if (NULL == unit) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret), KP(unit));
      } else if (OB_FAIL(output_delete_unit_ptr_array.push_back(unit))) {
        LOG_WARN("fail to push back", K(ret));
      } else {} // no more to do
    }
  } else {
    common::ObArray<ZoneUnitPtr> zone_unit_ptrs;
    if (OB_UNLIKELY(NULL == pool)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid argument", K(ret), KP(pool));
    } else if (OB_FAIL(build_sorted_zone_unit_ptr_array(pool, zone_unit_ptrs))) {
      LOG_WARN("fail to build sorted zone unit ptr array", K(ret));
    } else if (zone_unit_ptrs.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("zone unit ptrs has no element", K(ret), "zone_unit_cnt", zone_unit_ptrs.count());
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < zone_unit_ptrs.count(); ++i) {
        const ZoneUnitPtr &zone_unit_ptr = zone_unit_ptrs.at(i);
        for (int64_t j = alter_unit_num; OB_SUCC(ret) && j < zone_unit_ptr.unit_ptrs_.count(); ++j) {
          ObUnit *unit = zone_unit_ptr.unit_ptrs_.at(j);
          if (OB_UNLIKELY(NULL == unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", K(ret), KP(unit), K(i), K(j));
          } else if (OB_FAIL(output_delete_unit_ptr_array.push_back(unit))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      }
    }
  }
  return ret;
}


// the resource pool don't grant any tenants shrink directly.
// step:
// 1. clear __all_unit, change the unit num in __all_resouce_pool
// 2. clear the unit info in memory structure, change the unit_num in memroy structure resource pool
int ObUnitManager::shrink_not_granted_pool(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_id_array)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (is_valid_tenant_id(pool->tenant_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool already grant to some tenant", K(ret), "tenant_id", pool->tenant_id_);
  } else if (pool->unit_count_ <= alter_unit_num) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should be a shrink pool operation", K(ret),
             "cur_unit_num", pool->unit_count_,
             "new_unit_num", alter_unit_num);
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("start transaction failed", K(ret));
  } else {
    common::ObArray<ObUnit *> output_delete_unit_ptr_array;
    common::ObArray<uint64_t> output_delete_unit_id_array;
    share::ObResourcePool new_pool;
    if (OB_FAIL(new_pool.assign(*pool))) {
      LOG_WARN("fail to assign new pool", K(ret));
    } else {
      new_pool.unit_count_ = alter_unit_num;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
      LOG_WARN("fail to update resource pool", K(ret));
    } else if (OB_FAIL(check_shrink_unit_num_zone_condition(pool, alter_unit_num, delete_unit_id_array))) {
      LOG_WARN("fail to check shrink unit num zone condition", K(ret));
    } else if (OB_FAIL(fill_delete_unit_ptr_array(
            pool, delete_unit_id_array, alter_unit_num, output_delete_unit_ptr_array))) {
      LOG_WARN("fail to fill delete unit id array", K(ret));
    } else if (output_delete_unit_ptr_array.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("zone unit ptrs has no element", K(ret),
               "zone_unit_cnt", output_delete_unit_ptr_array.count());
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < output_delete_unit_ptr_array.count(); ++i) {
        const ObUnit *unit = output_delete_unit_ptr_array.at(i);
        if (OB_UNLIKELY(NULL == unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret), KP(unit));
        } else if (OB_FAIL(ut_operator_.remove_unit(trans, *unit))) {
          LOG_WARN("fail to remove unit", K(ret), "unit", *unit);
        } else if (OB_FAIL(output_delete_unit_id_array.push_back(unit->unit_id_))) {
          LOG_WARN("fail to push back", K(ret));
        } else {} // no more to do
      }
    }
    // however, we need to end this transaction
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("trans end failed", K(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(delete_inmemory_units(pool->resource_pool_id_, output_delete_unit_id_array))) {
      LOG_WARN("fail to delete unit groups", K(ret));
    } else {
      pool->unit_count_ = alter_unit_num;
    }
  }
  return ret;
}

int ObUnitManager::check_shrink_granted_pool_allowed_by_migrate_unit(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  UNUSED(alter_unit_num);
  is_allowed = true;
  common::ObArray<share::ObUnit *> *units = NULL;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unit ptr is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && is_allowed && i < units->count(); ++i) {
      const ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret), KP(unit));
      } else if (unit->migrate_from_server_.is_valid()) {
        is_allowed = false;
      } else {} // unit not in migrating, check next
    }
  }
  return ret;
}

int ObUnitManager::check_all_pools_granted(
    const common::ObIArray<share::ObResourcePool *> &pools,
    bool &all_granted)
{
  int ret = OB_SUCCESS;
  all_granted = true;
  for (int64_t i = 0; all_granted && OB_SUCC(ret) && i < pools.count(); ++i) {
    if (NULL == pools.at(i)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool ptr is null", K(ret), K(i));
    } else if (!is_valid_tenant_id(pools.at(i)->tenant_id_)) {
      all_granted = false;
    } else {} // go on check next
  }
  return ret;
}

//alter 14x add the new locality F/L@ZONE;
//temporarily logonly_unit = 1 does not support scaling,
//so only the unit num of non-logonly_unit can be reduced;
//Here it is only judged whether the number of units on non-logonly_unit satisfies locality;
//The goal of this function is to determine whether the distribution of the current unit meets the requirements of locality
//check in two parts:
//1. First check whether the logonly unit meets the requirements
//2. second check whether the non-logonly unit meets the requirements
int ObUnitManager::do_check_shrink_granted_pool_allowed_by_locality(
    const common::ObIArray<share::ObResourcePool *> &pools,
    const common::ObIArray<common::ObZone> &schema_zone_list,
    const ZoneLocalityIArray &zone_locality,
    const ObIArray<int64_t> &new_unit_nums,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  UNUSED(schema_zone_list);
  is_allowed = true;
  common::hash::ObHashMap<common::ObZone, UnitNum> zone_unit_num_map;
  const int64_t BUCKET_SIZE = 2 * MAX_ZONE_NUM;
  if (OB_UNLIKELY(pools.count() <= 0
                  || new_unit_nums.count() <= 0
                  || new_unit_nums.count() != pools.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(pools.count()), K(new_unit_nums.count()));
  } else if (OB_FAIL(zone_unit_num_map.create(BUCKET_SIZE, ObModIds::OB_RS_UNIT_MANAGER))) {
    LOG_WARN("fail to create map", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      const int64_t pool_unit_num = new_unit_nums.at(i);
      const share::ObResourcePool *pool = pools.at(i);
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret));
      } else {
        const common::ObIArray<common::ObZone> &pool_zone_list = pool->zone_list_;
        for (int64_t j = 0; OB_SUCC(ret) && j < pool_zone_list.count(); ++j) {
          const common::ObZone &zone = pool_zone_list.at(j);
          const int32_t overwrite = 1;
          UnitNum unit_num_set;
          int tmp_ret = zone_unit_num_map.get_refactored(zone, unit_num_set);
          if (OB_SUCCESS == tmp_ret) {
            // bypass
          } else if (OB_HASH_NOT_EXIST == tmp_ret) {
            unit_num_set.reset();
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("fail to get from map", K(ret));
          }
          if (OB_FAIL(ret)) {
          } else if (REPLICA_TYPE_FULL == pool->replica_type_) {
            unit_num_set.full_unit_num_ += pool_unit_num;
          } else if (REPLICA_TYPE_LOGONLY == pool->replica_type_) {
            unit_num_set.logonly_unit_num_ += pool_unit_num;
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("pool replica type unexpected", K(ret), "pool", *pool);
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(zone_unit_num_map.set_refactored(zone, unit_num_set, overwrite))) {
            LOG_WARN("fail to set map", K(ret));
          }
        }
      }
    }
    for (int64_t i = 0; is_allowed && OB_SUCC(ret) && i < zone_locality.count(); ++i) {
      const share::ObZoneReplicaAttrSet &zone_replica_attr = zone_locality.at(i);
      int64_t full_unit_num = 0;
      int64_t logonly_unit_num = 0;
      for (int64_t j = 0; OB_SUCC(ret) && j < zone_replica_attr.zone_set_.count(); ++j) {
        const common::ObZone &zone = zone_replica_attr.zone_set_.at(j);
        UnitNum unit_num_set;
        if (OB_FAIL(zone_unit_num_map.get_refactored(zone, unit_num_set))) {
          LOG_WARN("fail to get refactored", K(ret));
        } else {
          full_unit_num += unit_num_set.full_unit_num_;
          logonly_unit_num += unit_num_set.logonly_unit_num_;
        }
      }
      if (OB_SUCC(ret)) {
        const int64_t specific_num = zone_replica_attr.get_specific_replica_num();
        const int64_t except_l_specific_num = specific_num
                                              - zone_replica_attr.get_logonly_replica_num()
                                              - zone_replica_attr.get_encryption_logonly_replica_num();
        const int64_t total_unit_num = full_unit_num + logonly_unit_num;
        is_allowed = (total_unit_num >= specific_num && full_unit_num >= except_l_specific_num);
      }
    }
  }
  return ret;
}

// Scaling also needs to consider the locality of the tenant or table under the tenant.
// There are several situations that need to be considered:
// 1 Although the resource pool has been granted to a tenant,
//   but the zone of the resource pool may not be in the tenant's zone list.
//   For example: tenant has two pools : pool1(z1,z2,z3),pool2(z4,z5,z6),
//                The locality of tenant is F@z1,F@z2,F@z3,F@z4,
//                tenant the zone_list is (z1, z2, z3, z4). In this case,
//                only z4 needs to be considered when shrinkg pool2 unit num.
// 2 The locality of the tenant may include zone locality and region locality.
//   For zone locality, it is only necessary to consider that
//   the unit num of the zone after scaling is not less than the number of copies of the locality in this zone.
// 3 For region locality,
//   it is necessary to satisfy that the sum of unit num of all zones in the region under
//   this tenant is not less than the number of locality replicas on the region.
//   For example: tenant has two pools, pool1(z1,z2) and pool2(z3,z4),
//                the current pool1_unit_num = 2, pool2_unit_num = 2,
//                where z1, z4 belong to region SH, z2, z3 belong to region HZ, and locality is F{2},R{2}@SH,F{2},R{2}@HZ.
//   We try to compress the unit_num of pool2 to 1,
//   the total number of units in region SH after compression is 3,
//   and the total number of units in region HZ is 3,
//   which is not enough to accommodate the number of copies of locality.
//   Therefore, compressing the unit num of pool2 to 1 is not allowed.
//The specific implementation is to traverse each zone in the zone list of the resource pool:
//1 The zone does not exist in the tenant's zone list, skip it directly;
//2 The zone exists in the tenant's zone locality.
//  It is necessary to compare whether the compressed unit num is enough to accommodate
//  the number of locality replicas in the zone;
//3 If the zone does not exist in the tenant's zone locality, save the unit num in the region_unit_num_container.
//4 Save the zones of other resource pools under the tenant to the region zone container,
//  and compare whether the unit num is sufficient
int ObUnitManager::check_shrink_granted_pool_allowed_by_tenant_locality(
    share::schema::ObSchemaGetterGuard &schema_guard,
    const uint64_t tenant_id,
    const common::ObIArray<share::ObResourcePool *> &pools,
    const common::ObIArray<int64_t> &new_unit_nums,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  common::ObArray<common::ObZone> tenant_zone_list;
  if (OB_UNLIKELY(! is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), K(tenant_id));
  } else if (NULL == tenant_schema) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant schema is null", K(ret), KP(tenant_schema));
  } else if (OB_FAIL(tenant_schema->get_zone_list(tenant_zone_list))) {
    LOG_WARN("fail to get zone list", K(ret));
  } else {
    ObArray<ObZoneReplicaNumSet> zone_locality;
    if (OB_FAIL(tenant_schema->get_zone_replica_attr_array(zone_locality))) {
      LOG_WARN("fail to assign", K(ret));
    } else if (OB_FAIL(do_check_shrink_granted_pool_allowed_by_locality(
                pools, tenant_zone_list, zone_locality, new_unit_nums, is_allowed))) {
      LOG_WARN("fail to do check shrink by locality", K(ret));
    } else {} // no more to do
  }
  return ret;
}

int ObUnitManager::filter_logonly_task(const common::ObIArray<share::ObResourcePool *> &pools,
                                       ObIArray<share::ObZoneReplicaNumSet> &zone_locality)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnitInfo> unit_info;
  ObArray<ObZone> logonly_zone;
  ObArray<ObUnitInfo> tmp_unit_info;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  }

  for (int64_t i = 0; i < pools.count() && OB_SUCC(ret); i++) {
    tmp_unit_info.reset();
    if (REPLICA_TYPE_LOGONLY != pools.at(i)->replica_type_) {
      //nothing todo
    } else if (OB_FAIL(inner_get_unit_infos_of_pool(pools.at(i)->resource_pool_id_, tmp_unit_info))) {
      LOG_WARN("fail to get unit info", K(ret), K(i));
    } else {
      for (int64_t j = 0; j < tmp_unit_info.count() && OB_SUCC(ret); j++) {
        if (OB_FAIL(logonly_zone.push_back(tmp_unit_info.at(j).unit_.zone_))) {
          LOG_WARN("fail to push back", K(ret));
        }
      }
    }
  }

  for (int64_t i = 0; i < zone_locality.count() && OB_SUCC(ret); ++i) {
    share::ObZoneReplicaAttrSet &zone_replica_attr_set = zone_locality.at(i);
    if (zone_replica_attr_set.get_logonly_replica_num()
        + zone_replica_attr_set.get_encryption_logonly_replica_num() <= 0) {
      // nothing todo
    } else if (zone_replica_attr_set.zone_set_.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("zone set unexpected", K(ret), K(zone_replica_attr_set));
    } else {
      for (int64_t j = 0; j < logonly_zone.count(); j++) {
        const ObZone &zone = logonly_zone.at(j);
        if (!has_exist_in_array(zone_replica_attr_set.zone_set_, zone)) {
          // bypass
        } else if (zone_replica_attr_set.get_logonly_replica_num()
            + zone_replica_attr_set.get_encryption_logonly_replica_num() <= 0) {
          // bypass
        } else if (zone_replica_attr_set.get_logonly_replica_num() > 0) {
          ret = zone_replica_attr_set.sub_logonly_replica_num(ReplicaAttr(1, 100));
        } else {
          ret = zone_replica_attr_set.sub_encryption_logonly_replica_num(ReplicaAttr(1, 100));
        }
      }
    }
  }
  return ret;
}

//  Currently supports tenant-level and table-level locality,
//  and check shrinkage needs to be checked at the same time two levels of locality, tenant and table.
//  1 The tenant level is more intuitive.
//    directly compare the zone/region locality involved with the reduced unit num.
//  2 Three situations need to be considered at the table level:
//    2.1 Virtual tables, index tables, etc. without entity partitions, ignore them
//    2.2 The table with entity partition whose table locality is empty,
//        inherited from tenant, has been checked, and skipped;
//    2.3 Tables with entity partitions whose table locality is not empty are checked according to table locality
int ObUnitManager::check_shrink_granted_pool_allowed_by_locality(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  common::ObArray<share::ObResourcePool *> *pool_list = nullptr;
  common::ObArray<share::ObResourcePool *> new_pool_list;
  common::ObArray<int64_t> new_unit_nums;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (OB_ISNULL(schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service is null", K(schema_service_), KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(pool->tenant_id_, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(pool->tenant_id_));
  } else if (OB_FAIL(schema_guard.get_tenant_info(pool->tenant_id_, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), "tenant_id", pool->tenant_id_);
  } else if (OB_UNLIKELY(NULL == tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant schema is null", K(ret), KP(tenant_schema));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_schema->get_tenant_id(), pool_list))) {
    LOG_WARN("fail to get pools by tenant", K(ret));
  } else if (OB_UNLIKELY(nullptr == pool_list)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool list ptr is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pool_list->count(); ++i) {
      share::ObResourcePool *this_pool = pool_list->at(i);
      if (OB_UNLIKELY(nullptr == this_pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("this pool ptr is null", K(ret));
      } else if (OB_FAIL(new_pool_list.push_back(this_pool))) {
        LOG_WARN("fail to to push back", K(ret));
      } else if (OB_FAIL(new_unit_nums.push_back(
              this_pool->resource_pool_id_ == pool->resource_pool_id_
              ? alter_unit_num : this_pool->unit_count_))) {
        LOG_WARN("fail to push back", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(check_shrink_granted_pool_allowed_by_tenant_locality(
            schema_guard, pool->tenant_id_, new_pool_list, new_unit_nums, is_allowed))) {
      LOG_WARN("fail to check shrink by tenant locality", K(ret));
    } else {} // no more to do
  }
  return ret;
}

int ObUnitManager::check_shrink_granted_pool_allowed_by_alter_locality(
    share::ObResourcePool *pool,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  rootserver::ObRootService *root_service = NULL;
  bool in_alter_locality = true;
  if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(pool->tenant_id_))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant id", K(ret), "tenant_id", pool->tenant_id_);
  } else if (OB_UNLIKELY(NULL == (root_service = GCTX.root_service_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("rootservice is null", K(ret));
  } else if (OB_FAIL(root_service->check_tenant_in_alter_locality(
          pool->tenant_id_, in_alter_locality))) {
    LOG_WARN("fail to check tenant in alter locality", K(ret), "tenant_id", pool->tenant_id_);
  } else if (in_alter_locality) {
    is_allowed = false;
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "shrink pool unit num while altering locality");
  } else {} // no more to do
  return ret;
}

//  The shrinking operation of the granted resource pool requires the following pre-checks
//  1 The design avoids simultaneous migration and shrinkage of units.
//    Therefore, it is necessary to check whether all the units contained in the current resource pool are in the migration state before shrinking.
//    If so, shrinking operations are temporarily not allowed, and the migration needs to be completed.
//  2 The scaling operation also needs to consider locality.
//    The number of copies of locality cannot exceed the number of units after scaling.
//    For example, when the locality is F, R@zone1, the unit num in zone1 cannot be reduced to 1.
//  3 Locality changes and scaling under the same tenant are not allowed to be performed at the same time
int ObUnitManager::check_shrink_granted_pool_allowed(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    bool &is_allowed)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (OB_GTS_TENANT_ID == pool->tenant_id_) {
    is_allowed = true;
  } else if (OB_FAIL(check_shrink_granted_pool_allowed_by_migrate_unit(pool, alter_unit_num, is_allowed))) {
    LOG_WARN("fail to check by migrate unit", K(ret));
  } else if (!is_allowed) {
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "shrink pool unit num while unit migrating");
  } else if (OB_FAIL(check_shrink_granted_pool_allowed_by_locality(pool, alter_unit_num, is_allowed))) {
    LOG_WARN("fail to check by locality", K(ret));
  } else if (!is_allowed) {
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "shrink pool unit num causing not enough locality");
  } else if (OB_FAIL(check_shrink_granted_pool_allowed_by_alter_locality(pool, is_allowed))) {
    LOG_WARN("fail to check shrink granted pool allowed by alter locality", K(ret));
  } else if (!is_allowed) {
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "shrink pool unit num while altering locality");
  }
  return ret;
}

int ObUnitManager::register_shrink_pool_unit_num_rs_job(
     const uint64_t resource_pool_id,
     const uint64_t tenant_id,
     const int64_t new_unit_num,
	   common::ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  const int64_t extra_info_len = common::MAX_ROOTSERVICE_EVENT_EXTRA_INFO_LENGTH;
  char extra_info[common::MAX_ROOTSERVICE_EVENT_EXTRA_INFO_LENGTH] = {0};
  if (OB_SUCCESS != (ret = databuff_printf(extra_info, extra_info_len, pos,
          "new_unit_num: %ld", new_unit_num))) {
    if (OB_SIZE_OVERFLOW == ret) {
      LOG_WARN("format to buff size overflow", K(ret));
    } else {
      LOG_WARN("format new unit num failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    int64_t job_id = RS_JOB_CREATE(SHRINK_RESOURCE_POOL_UNIT_NUM, trans,
                                   "resource_pool_id", resource_pool_id,
                                   "tenant_id", tenant_id,
                                   "extra_info", extra_info);
    if (job_id < 1) {
      ret = OB_SQL_OPT_ERROR;
      LOG_WARN("insert into all_rootservice_job failed", K(ret));
    }
  }
  return ret ;
}

int ObUnitManager::rollback_shrink_pool_unit_num_rs_job(
     const uint64_t resource_pool_id,
     const uint64_t tenant_id,
     common::ObMySQLTransaction &trans)
{
  ObRsJobInfo job_info;
  int ret = RS_JOB_FIND(job_info, trans,
                        "job_type", "SHRINK_RESOURCE_POOL_UNIT_NUM",
                        "job_status", "INPROGRESS",
                        "resource_pool_id", resource_pool_id,
                        "tenant_id", tenant_id);
  if (OB_SUCC(ret) && job_info.job_id_ > 0) {
    if (OB_FAIL(RS_JOB_COMPLETE(job_info.job_id_, -1, trans))) { // Roll back, this shrink failed
      LOG_WARN("update all_rootservice_job failed", K(ret), K(job_info));
    }
  } else {
    LOG_WARN("failed to find rs job", K(ret), "tenant_id", tenant_id);
  }
  return ret;
}

int ObUnitManager::complete_shrink_pool_unit_num_rs_job(
     const uint64_t resource_pool_id,
     const uint64_t tenant_id,
	 common::ObMySQLTransaction &trans)

{
  ObRsJobInfo job_info;
  int ret = RS_JOB_FIND(job_info, trans,
                        "job_type", "SHRINK_RESOURCE_POOL_UNIT_NUM",
                        "job_status", "INPROGRESS",
                        "resource_pool_id", resource_pool_id,
                        "tenant_id", tenant_id);
  if (OB_SUCC(ret) && job_info.job_id_ > 0) {
    if (OB_FAIL(RS_JOB_COMPLETE(job_info.job_id_, 0, trans))) { // job success
      LOG_WARN("update all_rootservice_job failed", K(ret), K(job_info));
    }
  } else {
    LOG_WARN("failed to find rs job", K(ret), "tenant_id" K(tenant_id));
  }
  return ret ;
}
int ObUnitManager::complete_migrate_unit_rs_job_in_pool(
      const int64_t resource_pool_id,
      const int result_ret,
      common::ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnit *> *units = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(resource_pool_id), K(ret));
  } else if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
    LOG_WARN("fail to get unit by pool", K(resource_pool_id), K(ret));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units is null", KP(units), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      const ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit should not be null", K(ret));
      } else if (unit->is_manual_migrate()) {
        char ip_buf[common::MAX_IP_ADDR_LENGTH];
        (void)unit->server_.ip_to_string(ip_buf, common::MAX_IP_ADDR_LENGTH);
        ObRsJobInfo job_info;
        int tmp_ret = RS_JOB_FIND(job_info, *proxy_, "job_type", "MIGRATE_UNIT",
                                  "job_status", "INPROGRESS",
                                  "unit_id", unit->unit_id_,
                                  "svr_ip", ip_buf,
                                  "svr_port", unit->server_.get_port());
        if (OB_SUCCESS == tmp_ret && job_info.job_id_ > 0) {
          if (OB_FAIL(RS_JOB_COMPLETE(job_info.job_id_, result_ret, trans))) {
            LOG_WARN("all_rootservice_job update failed", K(ret), K(result_ret), K(job_info));
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::shrink_granted_pool(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_id_array)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  bool is_allowed = true;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (!is_valid_tenant_id(pool->tenant_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool not grant to any tenant", K(ret), "tenant_id", pool->tenant_id_);
  } else if (pool->unit_count_ <= alter_unit_num) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should be a shrink pool operation", K(ret),
             "cur_unit_num", pool->unit_count_,
             "new_unit_num", alter_unit_num);
  } else if (OB_FAIL(check_shrink_granted_pool_allowed(pool, alter_unit_num, is_allowed))) {
    LOG_WARN("fail to check shrink granted pool allowed",
             K(ret), "pool", *pool, K(alter_unit_num));
  } else if (!is_allowed) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("shrink granted pool is not allowed", K(ret), "pool", *pool);
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("start transaction failed", K(ret));
  } else if (OB_FAIL(register_shrink_pool_unit_num_rs_job(
          pool->resource_pool_id_, pool->tenant_id_, alter_unit_num, trans))) {
    LOG_WARN("do rs_job failed ", K(ret));
  } else {
    common::ObArray<ObUnit *> output_delete_unit_ptr_array;
    share::ObResourcePool new_pool;
    if (OB_FAIL(new_pool.assign(*pool))) {
      LOG_WARN("fail to assign new pool", K(ret));
    } else {
      new_pool.unit_count_ = alter_unit_num;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
      LOG_WARN("fail to update resource pool", K(ret));
    } else if (OB_FAIL(check_shrink_unit_num_zone_condition(pool, alter_unit_num, delete_unit_id_array))) {
      LOG_WARN("fail to check shrink unit num zone condition", K(ret));
    } else if (OB_FAIL(fill_delete_unit_ptr_array(
            pool, delete_unit_id_array, alter_unit_num, output_delete_unit_ptr_array))) {
      LOG_WARN("fail to fill delete unit id array", K(ret));
    } else if (output_delete_unit_ptr_array.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("zone unit ptrs has no element", K(ret),
               "zone_unit_cnt", output_delete_unit_ptr_array.count());
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < output_delete_unit_ptr_array.count(); ++i) {
        const ObUnit *unit = output_delete_unit_ptr_array.at(i);
        if (OB_UNLIKELY(NULL == unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret), KP(unit), K(i));
        } else {
          ObUnit new_unit = *unit;
          new_unit.status_ = ObUnit::UNIT_STATUS_DELETING;
          if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
            LOG_WARN("fail to update unit", K(ret), K(new_unit));
          } else {} // no more to do
        }
      }
    }
    // however, we need to end this transaction
    const bool commit = (OB_SUCCESS == ret);
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("trans end failed", K(tmp_ret), K(commit));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_FAIL(ret)) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < output_delete_unit_ptr_array.count(); ++i) {
        ObUnit *unit = output_delete_unit_ptr_array.at(i);
        if (OB_UNLIKELY(NULL == unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret));
        } else {
          unit->status_ = ObUnit::UNIT_STATUS_DELETING;
        }
      }
      if (OB_SUCC(ret)) {
        pool->unit_count_ = alter_unit_num;
      }
    }
  }
  return ret;
}

// Roll back the pool shrinkage and directly adjust the unit num and unit status of the resource pool to active.
int ObUnitManager::rollback_shrink_pool_unit_num(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *units = NULL;
  common::ObMySQLTransaction trans;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(alter_unit_num));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("start transaction failed", K(ret));
  } else if (OB_FAIL(rollback_shrink_pool_unit_num_rs_job(pool->resource_pool_id_, pool->tenant_id_, trans))) {
    LOG_WARN("rollback rs_job failed ", K(ret));
  } else {
    if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
      LOG_WARN("failt to get units by pool", K(ret), "pool_id", pool->resource_pool_id_);
    } else if (OB_UNLIKELY(NULL == units)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("units is null", K(ret), KP(units), "pool_id", pool->resource_pool_id_);
    } else if (units->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("total unit num unexpected", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
        const ObUnit *this_unit = units->at(i);
        if (OB_UNLIKELY(NULL == this_unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret));
        } else if (ObUnit::UNIT_STATUS_ACTIVE == this_unit->status_) {
          // go and process the unit in deleting
        } else if (ObUnit::UNIT_STATUS_DELETING == this_unit->status_) {
          ObUnit new_unit = *this_unit;
          new_unit.status_ = ObUnit::UNIT_STATUS_ACTIVE;
          if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
            LOG_WARN("fail to update unit", K(ret), K(new_unit), "cur_unit", *this_unit);
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected unit status", K(ret), "unit", *this_unit);
        }
      }
      if (OB_SUCC(ret)) {
        share::ObResourcePool new_pool;
        if (OB_FAIL(new_pool.assign(*pool))) {
          LOG_WARN("fail to assign new pool", K(ret));
        } else {
          new_pool.unit_count_ = alter_unit_num;
          if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
            LOG_WARN("update resource pool failed", K(new_pool), K(ret));
          }
        }
      }
      const bool commit = (OB_SUCCESS == ret);
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
        LOG_WARN("trans end failed", K(commit), K(tmp_ret));
        ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
      }

      if (OB_SUCC(ret)) {
        for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
          ObUnit *this_unit = units->at(i);
          if (OB_UNLIKELY(NULL == this_unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", K(ret), KP(this_unit));
          } else if (ObUnit::UNIT_STATUS_ACTIVE == this_unit->status_) {
            // go and process the unit in deleting
          } else if (ObUnit::UNIT_STATUS_DELETING == this_unit->status_) {
            this_unit->status_ = ObUnit::UNIT_STATUS_ACTIVE;
          } else {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected unit status", K(ret), "unit", *this_unit);
          }
        }
        if (OB_SUCC(ret)) {
          pool->unit_count_ = alter_unit_num;
        }
      }
    }
  }
  return ret;
}

/* the routine that calls the expand_pool_unit_num shall
 * ensure that this pool is not granted to any tenant
 */
int ObUnitManager::expand_pool_unit_num_(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num)
{
  int ret = OB_SUCCESS;
  const char *module = "ALTER_RESOURCE_POOL_UNIT_NUM";
  const bool new_allocate_pool = false;
  common::ObMySQLTransaction trans;
  ObArray<ObAddr> new_servers;
  ObArray<share::ObUnit *> *units = NULL;
  ObArray<uint64_t> bak_unit_ids;
  if (OB_UNLIKELY(NULL == pool || alter_unit_num <= pool->unit_count_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool ptr is null", K(ret), "pre_unit_cnt", pool->unit_count_, "cur_unit_cnt", alter_unit_num);
  } else if (pool->is_granted_to_tenant()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected pool status", KR(ret), KPC(pool));
  } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
    LOG_WARN("start transaction failed", K(ret));
  } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret));
  } else if (OB_FAIL(extract_unit_ids(*units, bak_unit_ids))) {
    LOG_WARN("fail to extract unit ids", K(ret));
  } else if (OB_FAIL(allocate_pool_units_(
          trans, *pool, pool->zone_list_, nullptr, new_allocate_pool,
          alter_unit_num - pool->unit_count_, module, new_servers))) {
    LOG_WARN("arrange units failed", "pool", *pool, K(new_allocate_pool), K(ret));
  } else {
    share::ObResourcePool new_pool;
    if (OB_FAIL(new_pool.assign(*pool))) {
      LOG_WARN("failed to assign new_poll", K(ret));
    } else {
      new_pool.unit_count_ = alter_unit_num;
      if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
        LOG_WARN("update_resource_pool failed", K(new_pool), K(ret));
      }
    }
  }

  if (trans.is_started()) {
    const bool commit = (OB_SUCC(ret));
    int temp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (temp_ret = trans.end(commit))) {
      LOG_WARN("trans end failed", K(commit), K(temp_ret));
      ret = (OB_SUCCESS == ret) ? temp_ret : ret;
    }
  }

  if (OB_FAIL(ret)) {
    // avoid overwrite ret
    int64_t temp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (temp_ret = delete_invalid_inmemory_units(pool->resource_pool_id_, bak_unit_ids))) {
      LOG_WARN("delete unit groups failed", "resource pool id", pool->resource_pool_id_, K(temp_ret));
    }
  } else {
    pool->unit_count_ = alter_unit_num;
  }
  return ret;
}

int ObUnitManager::get_pool_complete_unit_num_and_status(
    const share::ObResourcePool *pool,
    int64_t &unit_num_per_zone,
    int64_t &current_unit_num_per_zone,
    bool &has_unit_num_modification)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit *> *units = NULL;
  if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is null", K(ret));
  } else if (OB_FAIL(get_units_by_pool(pool->resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret), "pool_id", pool->resource_pool_id_);
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units is null", K(ret), KP(units), "pool_id", pool->resource_pool_id_);
  } else if (units->count() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("total unit num unexpected", K(ret), "total_unit_num", units->count());
  } else if (pool->zone_list_.count() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool zone list count unexpected", K(ret), "pool_zone_list_count", pool->zone_list_.count());
  } else {
    int64_t total_unit_cnt = units->count();
    int64_t zone_cnt = pool->zone_list_.count();
    int64_t consult = total_unit_cnt / zone_cnt;
    if (total_unit_cnt != consult * zone_cnt) {
      // total_unit_cnt cannot be divisible by zone_cnt,
      // the number of units in each zone of the resource pool is inconsistent
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected total unit cnt", K(ret), K(zone_cnt), K(total_unit_cnt));
    } else {
      unit_num_per_zone = consult;
      current_unit_num_per_zone = pool->unit_count_;
      has_unit_num_modification = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < units->count() && !has_unit_num_modification; ++i) {
        const ObUnit *this_unit = units->at(i);
        if (OB_UNLIKELY(NULL == this_unit)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit ptr is null", K(ret), KP(this_unit));
        } else if (ObUnit::UNIT_STATUS_ACTIVE == this_unit->status_) {
          // an active unit, go and check next
        } else if (ObUnit::UNIT_STATUS_DELETING == this_unit->status_) {
          has_unit_num_modification = true;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected unit status", K(ret), "unit_status", this_unit->status_);
        }
      }
    }
  }
  return ret;
}

// According to the unit num of the current resource pool
// and the unit_num(alter_unit_num) of the resource pool after the change,
// the change type of the current operation is determined.
// Let the current num be cur_unit_num, and the num after the change should be alter_unit_num.
// The principle is as follows:
// 1 cur_unit_num == alter_unit_num, no change, means no operation, no modification
// 2 cur_unit_num <alter_unit_num, and there is currently no modification operation of the resource pool and no other unit num,
//   which is considered to be an expansion operation.
// 3 cur_unit_num <alter_unit_num, and the resource pool is shrinking,
//   the unit_num before shrinking is prev_unit_num,
//   and alter_unit_num is equal to prev_unit_num,
//   which is considered to be a shrinking rollback operation.
// 4 cur_unit_num> alter_unit_num, and the resource pool has not performed other unit num modification operations,
//   which is considered to be a shrinking operation
int ObUnitManager::determine_alter_unit_num_type(
    share::ObResourcePool *pool,
    const int64_t alter_unit_num,
    AlterUnitNumType &alter_unit_num_type)
{
  int ret = OB_SUCCESS;
  int64_t complete_unit_num_per_zone = 0; // Contains the unit in the delete state
  int64_t current_unit_num_per_zone = 0; // unit num in pool->unit_count
  bool has_unit_num_modification = true;
  if (OB_UNLIKELY(NULL == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is null", K(ret));
  } else if (alter_unit_num == pool->unit_count_) {
    alter_unit_num_type = AUN_NOP; // do nothing
  } else if (OB_FAIL(get_pool_complete_unit_num_and_status(
          pool, complete_unit_num_per_zone,
          current_unit_num_per_zone, has_unit_num_modification))) {
    LOG_WARN("fail to get pool complete unit num and status", K(ret));
  } else if (OB_UNLIKELY(complete_unit_num_per_zone <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected complete_unit_num", K(ret), K(complete_unit_num_per_zone));
  } else {
    if (has_unit_num_modification) { // A unit num change is taking place
      if (alter_unit_num == complete_unit_num_per_zone) {
        alter_unit_num_type = AUN_ROLLBACK_SHRINK;
      } else {
        alter_unit_num_type = AUN_MAX;
      }
    } else {
      if (alter_unit_num > pool->unit_count_) {
        alter_unit_num_type = AUN_EXPAND;
      } else if (alter_unit_num < pool->unit_count_) {
        alter_unit_num_type = AUN_SHRINK;
      } else {
        alter_unit_num_type = AUN_NOP;
      }
    }
  }
  return ret;
}

int ObUnitManager::alter_pool_unit_num(
    share::ObResourcePool *pool,
    int64_t alter_unit_num,
    const common::ObIArray<uint64_t> &delete_unit_id_array)
{
  int ret = OB_SUCCESS;
  AlterUnitNumType alter_unit_num_type = AUN_MAX;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is null", K(ret));
  } else if (alter_unit_num <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid alter_unit_num", K(alter_unit_num), K(ret));
  } else if (pool->is_granted_to_tenant()) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("alter pool unit num which is granted to a tenant is not allowed", KR(ret), KPC(pool));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "alter pool unit num which is granted to a tenant");
  } else if (OB_FAIL(determine_alter_unit_num_type(pool, alter_unit_num, alter_unit_num_type))) {
    LOG_WARN("fail to determine alter unit num type", K(ret));
  } else if (AUN_NOP == alter_unit_num_type) {
    // do nothing
    if (delete_unit_id_array.count() > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("do not support deleting unit without unit num changed", K(ret));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "delete unit without unit num change");
    } else {} // good
  } else if (AUN_EXPAND == alter_unit_num_type) {
    if (delete_unit_id_array.count() > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("do not support expand pool unit num combined with deleting unit", K(ret));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "expand pool unit num combined with deleting unit");
    } else if (OB_FAIL(expand_pool_unit_num_(pool, alter_unit_num))) {
      LOG_WARN("fail to expend pool unit num", K(ret), K(alter_unit_num));
    }
  } else if (AUN_SHRINK == alter_unit_num_type) {
    if (OB_FAIL(shrink_pool_unit_num(pool, alter_unit_num, delete_unit_id_array))) {
      LOG_WARN("fail to shrink pool unit num", K(ret), K(alter_unit_num));
    }
  } else if (AUN_ROLLBACK_SHRINK == alter_unit_num_type) {
    if (delete_unit_id_array.count() > 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("do not support rollback shrink pool unit num combined with deleting unit", K(ret));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "rollback shrink pool unit num combined with deleting unit");
    } else if (OB_FAIL(rollback_shrink_pool_unit_num(pool, alter_unit_num))) {
      LOG_WARN("fail to rollbakc shrink pool unit num", K(ret), K(alter_unit_num));
    }
  } else if (AUN_MAX == alter_unit_num_type) {
    ret = OB_OP_NOT_ALLOW;
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "alter unit num while the previous operation is in progress");
    LOG_WARN("alter unit num not allowed", K(ret));
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected alter unit num type", K(ret), K(alter_unit_num_type));
  }
  return ret;
}

int ObUnitManager::get_zone_pools_unit_num(
    const common::ObZone &zone,
    const common::ObIArray<share::ObResourcePoolName> &new_pool_name_list,
    int64_t &total_unit_num,
    int64_t &full_unit_num,
    int64_t &logonly_unit_num)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret));
  } else if (zone.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(zone));
  } else {
    common::ObArray<share::ObResourcePool *> pool_list;
    for (int64_t i = 0; OB_SUCC(ret) && i < new_pool_name_list.count(); ++i) {
      const share::ObResourcePoolName &pool_name = new_pool_name_list.at(i);
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_name, pool))) {
        LOG_WARN("fail to get resource pool by name", K(ret));
      } else if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), K(pool_name));
      } else if (OB_FAIL(pool_list.push_back(pool))) {
        LOG_WARN("fail to push back", K(ret));
      } else {} // no more to do
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(inner_get_zone_pools_unit_num(
            zone, pool_list, total_unit_num, full_unit_num, logonly_unit_num))) {
      LOG_WARN("fail to inner get pools unit num", K(ret));
    }
  }
  return ret;
}

int ObUnitManager::check_can_remove_pool_zone_list(
    const share::ObResourcePool *pool,
    const common::ObIArray<common::ObZone> &to_be_removed_zones,
    bool &can_remove)
{
  int ret = OB_SUCCESS;
  can_remove = true;
  common::ObArray<share::ObResourcePool *> *pool_list = NULL;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  if (OB_UNLIKELY(NULL == pool) || OB_UNLIKELY(to_be_removed_zones.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(to_be_removed_zones));
  } else if (!pool->is_granted_to_tenant()) {
    can_remove = true; // this pool do not grant to any tenant, can remove zone unit
  } else if (OB_FAIL(get_pools_by_tenant(pool->tenant_id_, pool_list))) {
    LOG_WARN("fail to get pools by tenant", K(ret));
  } else if (OB_UNLIKELY(NULL == pool_list)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool list ptr is null", K(ret));
  } else if (OB_ISNULL(schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service is null", K(schema_service_), KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(pool->tenant_id_, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(pool->tenant_id_));
  } else if (OB_FAIL(schema_guard.get_tenant_info(pool->tenant_id_, tenant_schema))) {
    LOG_WARN("fail to get tenant schema", K(ret), "tenant_id", pool->tenant_id_);
  } else if (NULL == tenant_schema) {
    ret = OB_TENANT_NOT_EXIST;
    LOG_WARN("tenant schema is null", K(ret));
  } else if (!tenant_schema->get_previous_locality_str().empty()) {
    can_remove = false;
    LOG_WARN("alter pool zone list is not allowed while locality modification",
             "tenant_id", tenant_schema->get_tenant_id());
  } else {
    for (int64_t i = 0; can_remove && OB_SUCC(ret) && i < to_be_removed_zones.count(); ++i) {
      int64_t total_unit_num = 0;
      int64_t full_unit_num = 0;
      int64_t logonly_unit_num = 0;
      const common::ObZone &zone = to_be_removed_zones.at(i);
      bool enough = false;
      if (OB_FAIL(inner_get_zone_pools_unit_num(
              zone, *pool_list, total_unit_num, full_unit_num, logonly_unit_num))) {
        LOG_WARN("fail to get zone pools unit num", K(ret));
      } else if (total_unit_num != full_unit_num + logonly_unit_num) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit num value not match", K(ret),
                 K(total_unit_num), K(full_unit_num), K(logonly_unit_num));
      } else {
        // Reduce the unit of this pool on to be remove zone
        total_unit_num -= pool->unit_count_;
        if (common::REPLICA_TYPE_FULL == pool->replica_type_) {
          full_unit_num -= pool->unit_count_;
        } else if (common::REPLICA_TYPE_LOGONLY == pool->replica_type_) {
          logonly_unit_num -= pool->unit_count_;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("do not support this pool type", K(ret), K(*pool));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(inner_check_schema_zone_unit_enough(
            zone, total_unit_num, full_unit_num, logonly_unit_num,
            *tenant_schema, schema_guard, enough))) {
        LOG_WARN("fail to inner check schema zone unit enough", K(ret));
      } else if (!enough) {
        can_remove = false;
      } else {}
    }
  }
  return ret;
}

int ObUnitManager::inner_get_zone_pools_unit_num(
    const common::ObZone &zone,
    const common::ObIArray<share::ObResourcePool *> &pool_list,
    int64_t &total_unit_num,
    int64_t &full_unit_num,
    int64_t &logonly_unit_num)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K(ret));
  } else if (zone.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(zone));
  } else {
    total_unit_num = 0;
    full_unit_num = 0;
    logonly_unit_num = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < pool_list.count(); ++i) {
      share::ObResourcePool *pool = pool_list.at(i);
      if (OB_UNLIKELY(NULL == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret));
      } else if (!has_exist_in_array(pool->zone_list_, zone)) {
        //ignore
      } else {
        total_unit_num += pool->unit_count_;
        if (common::REPLICA_TYPE_FULL == pool->replica_type_) {
          full_unit_num += pool->unit_count_;
        } else if (common::REPLICA_TYPE_LOGONLY == pool->replica_type_) {
          logonly_unit_num += pool->unit_count_;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("do not support this pool type", K(ret), K(*pool));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::alter_pool_zone_list(
    share::ObResourcePool *pool,
    const ObIArray<ObZone> &zone_list)
{
  // The requirement of alter pool_zone_list is currently only for cluster relocation.
  // Before deleting the zone,
  // the pool_zone_list that contains the zone in all resource pools needs to be removed from the zone_list..
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(NULL == pool || zone_list.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), "new zone list", zone_list);
  } else {
    ObArray<ObZone> new_zone_list;
    //check repeat
    for (int64_t i = 0; i < zone_list.count() && OB_SUCC(ret); ++i) {
      if (has_exist_in_array(new_zone_list, zone_list.at(i))) {
        ret = OB_OP_NOT_ALLOW;
        LOG_USER_ERROR(OB_OP_NOT_ALLOW, "alter resource pool zone repeat");
        LOG_WARN("alter resource pool zone repeat not allow", K(ret));
      } else if (OB_FAIL(new_zone_list.push_back(zone_list.at(i)))) {
        LOG_WARN("fail to push back", K(ret));
      } else {}//no more to do
    }
    if (OB_SUCC(ret)) {
      std::sort(new_zone_list.begin(), new_zone_list.end());
      bool is_add_pool_zone = false;
      bool is_remove_pool_zone = false;
      for (int64_t i = 0; i < new_zone_list.count() && OB_SUCC(ret); ++i) {
        if (!has_exist_in_array(pool->zone_list_, new_zone_list.at(i))) {
          is_add_pool_zone = true;
        } else {}//nothing todo
      }
      for (int64_t i = 0; i < pool->zone_list_.count() && OB_SUCC(ret); ++i) {
        if (!has_exist_in_array(new_zone_list, pool->zone_list_.at(i))) {
          is_remove_pool_zone = true;
        } else {}//nothing todo
      }
      if (is_add_pool_zone && is_remove_pool_zone) {
        ret = OB_OP_NOT_ALLOW;
        LOG_USER_ERROR(OB_OP_NOT_ALLOW, "Cannot add and delete zones at the same time");
        LOG_WARN("Cannot add and delete zones at the same time", K(ret));
      } else if (is_add_pool_zone) {
        if (OB_FAIL(add_pool_zone_list(pool, new_zone_list))) {
          LOG_WARN("fail to add pool zone list", K(ret));
        }
      } else {
        if (OB_FAIL(remove_pool_zone_list(pool, new_zone_list))) {
          LOG_WARN("fail to remoce pool zone list", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::remove_pool_zone_list(
    share::ObResourcePool *pool,
    const ObIArray<ObZone> &zone_list)
{
  int ret = OB_SUCCESS;
  bool can_remove = false;
  common::ObArray<common::ObZone> zones_to_be_removed;
  const common::ObIArray<common::ObZone> &prev_zone_list = pool->zone_list_;
  if (OB_FAIL(cal_to_be_removed_pool_zone_list(
          prev_zone_list, zone_list, zones_to_be_removed))) {
    LOG_WARN("fail to calculate to be removed pool zone list", K(ret));
  } else if (zones_to_be_removed.count() <= 0) {
    // no zones need to be removed, return SUCC directly
  } else if (OB_FAIL(check_can_remove_pool_zone_list(pool, zones_to_be_removed, can_remove))) {
    LOG_WARN("fail to check can remove pool zon list", K(ret));
  } else if (!can_remove) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("cannot alter resource pool zone list", K(ret),
             "pool", *pool, K(zones_to_be_removed));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "alter resource pool zone list with not empty unit");
  } else if (OB_FAIL(do_remove_pool_zone_list(pool, zone_list, zones_to_be_removed))) {
    LOG_WARN("fail to do remove pool zone list", K(ret), "pool", *pool, K(zones_to_be_removed));
  } else {} // no more to do
  return ret;
}

int ObUnitManager::add_pool_zone_list(
    share::ObResourcePool *pool,
    const ObIArray<ObZone> &zone_list)
{
  int ret = OB_SUCCESS;
  bool can_add = false;
  common::ObArray<common::ObZone> zones_to_be_add;
  bool is_add_pool_zone_list_allowed = true;
  if (OB_UNLIKELY(nullptr == pool)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), KP(pool));
  } else if (! pool->is_granted_to_tenant()) {
    // good, can do add pool zone list directly
    is_add_pool_zone_list_allowed = true;
  } else {
    if (OB_FAIL(check_expand_zone_resource_allowed_by_old_unit_stat(
            pool->tenant_id_, is_add_pool_zone_list_allowed))) {
      LOG_WARN("fail to check grant pools allowed by unit stat",
               KR(ret), "tenant_id", pool->tenant_id_);
    }
  }

  if (OB_FAIL(ret)) {
    // bypass
  } else if (!is_add_pool_zone_list_allowed) {
    ret = OB_OP_NOT_ALLOW;
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "add pool zone when granted pools of tenant in shrinking");
  } else if (OB_FAIL(cal_to_be_add_pool_zone_list(
          pool->zone_list_, zone_list, zones_to_be_add))) {
    LOG_WARN("fail to calculate to be add pool zone list", K(ret));
  } else if (zones_to_be_add.count() <= 0) {
    //no zones need to be add, return SUCC directly
  } else if (OB_FAIL(check_can_add_pool_zone_list_by_locality(pool, zones_to_be_add, can_add))) {
    LOG_WARN("fail to check can add pool zone list", K(ret));
  } else if (!can_add) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("cannot alter resource pool zone list", K(ret),
             "pool", *pool, K(zones_to_be_add));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "alter resource pool zone list with not empty unit");
  } else if (OB_FAIL(do_add_pool_zone_list(pool, zone_list, zones_to_be_add))) {
    LOG_WARN("fail to do add pool zone list", K(ret), "pool", *pool, K(zones_to_be_add));
  } else {} // no more to do
  return ret;
}

int ObUnitManager::cal_to_be_add_pool_zone_list(
    const common::ObIArray<common::ObZone> &prev_zone_list,
    const common::ObIArray<common::ObZone> &cur_zone_list,
    common::ObIArray<common::ObZone> &to_be_add_zones) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(prev_zone_list.count() <= 0 || cur_zone_list.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret),
             "prev zone list", prev_zone_list,
             "cur zone list", cur_zone_list);
  } else {
    to_be_add_zones.reset();
    //Check if the added zone exists
    for (int64_t i = 0; OB_SUCC(ret) && i < cur_zone_list.count(); ++i) {
      const common::ObZone &this_zone = cur_zone_list.at(i);
      bool zone_exist = false;
      if (OB_FAIL(zone_mgr_.check_zone_exist(this_zone, zone_exist))) {
        LOG_WARN("failed to check zone exists", K(ret), K(this_zone));
      } else if (!zone_exist) {
        ret = OB_ZONE_INFO_NOT_EXIST;
        LOG_WARN("zone not exists", K(ret), K(this_zone));
      } else {
        if (has_exist_in_array(prev_zone_list, this_zone)) {
          //still exist, do nothing
        } else if (has_exist_in_array(to_be_add_zones, this_zone)) {
          //just add one time
          ret = OB_OP_NOT_ALLOW;
          LOG_USER_ERROR(OB_OP_NOT_ALLOW, "add repeat zone");
          LOG_WARN("not allow add repeat zone", K(ret));
        } else if (OB_FAIL(to_be_add_zones.push_back(this_zone))) {
          LOG_WARN("fail to push back", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_can_add_pool_zone_list_by_locality(
    const share::ObResourcePool *pool,
    const common::ObIArray<common::ObZone> &to_be_add_zones,
    bool &can_add)
{
  int ret = OB_SUCCESS;
  can_add = true;
  common::ObArray<share::ObResourcePool *> *pool_list = NULL;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  if (OB_UNLIKELY(NULL == pool) || OB_UNLIKELY(to_be_add_zones.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ivnalid argument", K(ret), KP(pool), K(to_be_add_zones));
  } else if (! pool->is_granted_to_tenant()) {
    can_add = true; // not in tenant, can add zone unit
  } else if (OB_FAIL(get_pools_by_tenant(pool->tenant_id_, pool_list))) {
    LOG_WARN("fail to get pools by tenant", K(ret));
  } else if (OB_ISNULL(schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service is null", K(schema_service_), KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(pool->tenant_id_, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(pool->tenant_id_));
  } else if (OB_FAIL(schema_guard.get_tenant_info(pool->tenant_id_, tenant_schema))) {
    LOG_WARN("fail to get tenant schema", K(ret), "tenant_id", pool->tenant_id_);
  } else if (NULL == tenant_schema) {
    ret = OB_SCHEMA_ERROR;
    LOG_WARN("tenant schema is null", K(ret));
  } else if (!tenant_schema->get_previous_locality_str().empty()) {//No locality changes are allowed at this time
    can_add = false;
    LOG_WARN("alter pool zone list is not allowed while locality modification",
             "tenant_id", tenant_schema->get_tenant_id());
  } else {} //nothing todo
  return ret;
}

int ObUnitManager::do_add_pool_zone_list(
    share::ObResourcePool *pool,
    const common::ObIArray<common::ObZone> &new_zone_list,
    const common::ObIArray<common::ObZone> &to_be_add_zones)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  share::ObResourcePool new_pool;
  const char *module = "ALTER_RESOURCE_POOL_ZONE_LIST";
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(NULL == pool)
             || OB_UNLIKELY(to_be_add_zones.count() <= 0)
             || OB_UNLIKELY(new_zone_list.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(to_be_add_zones), K(new_zone_list));
  } else {
    if (OB_FAIL(new_pool.assign(*pool))) {
      LOG_WARN("fail to assign new pool", K(ret));
    } else if (OB_FAIL(new_pool.zone_list_.assign(new_zone_list))) {
      LOG_WARN("fail to assign new pool zone list", K(ret));
    } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
      LOG_WARN("start transaction failed", K(ret));
    } else {
      if (OB_FAIL(increase_units_in_zones_(trans, *pool, to_be_add_zones, module))) {
        LOG_WARN("fail to add units in zones", K(module), KR(ret),
                 "pool id", pool->resource_pool_id_, K(to_be_add_zones));
      } else if (OB_FAIL(pool->zone_list_.assign(new_zone_list))) {
        LOG_WARN("fail to update pool zone list in memory", K(ret));
      } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
        LOG_WARN("fail to update resource pool", K(ret));
      } else {} //no more to do
      if (trans.is_started()) {
        const bool commit = (OB_SUCC(ret));
        int tmp_ret = OB_SUCCESS;
        if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
          LOG_WARN("fail to end trans", K(commit), K(tmp_ret));
          ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
        }
      } else {}//nothing todo
    }
  }
  return ret;
}

// allocate new units on new added zones for resource pool
int ObUnitManager::increase_units_in_zones_(
    common::ObISQLClient &client,
    share::ObResourcePool &pool,
    const common::ObIArray<common::ObZone> &to_be_add_zones,
    const char *module)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> new_unit_group_id_array;
  const bool new_allocate_pool = false;   // whether to alloacate new resource pool
  const int64_t new_unit_count = pool.unit_count_;  // new unit count on every zone
  ObArray<ObAddr> new_servers;

  for (int64_t i = 0; OB_SUCC(ret) && i < to_be_add_zones.count(); ++i) {
    bool zone_exist = false;
    if (OB_FAIL(zone_mgr_.check_zone_exist(to_be_add_zones.at(i), zone_exist))) {
      LOG_WARN("failed to check zone exists", K(ret), K(to_be_add_zones.at(i)));
    } else if (!zone_exist) {
      ret = OB_ZONE_INFO_NOT_EXIST;
      LOG_WARN("zone not exists", K(ret), K(to_be_add_zones.at(i)));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(get_pool_unit_group_id_(pool, new_unit_group_id_array))) {
    LOG_WARN("fail to get pool unit group id", KR(ret), K(pool));
  } else if (OB_FAIL(allocate_pool_units_(client, pool, to_be_add_zones, &new_unit_group_id_array,
      new_allocate_pool, new_unit_count, module, new_servers))) {
    LOG_WARN("fail to allocate new units for new added zones", K(module), KR(ret), K(to_be_add_zones), K(pool),
        K(new_unit_count), K(new_unit_group_id_array));
  }

  return ret;
}

int ObUnitManager::do_remove_pool_zone_list(
    share::ObResourcePool *pool,
    const common::ObIArray<common::ObZone> &new_zone_list,
    const common::ObIArray<common::ObZone> &to_be_removed_zones)
{
  int ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  share::ObResourcePool new_pool;
  common::ObArray<common::ObZone> new_zone_list1;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_UNLIKELY(NULL == pool)
             || OB_UNLIKELY(to_be_removed_zones.count() <= 0)
             || OB_UNLIKELY(new_zone_list.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), K(to_be_removed_zones), K(new_zone_list));
  } else {
    if (OB_FAIL(new_pool.assign(*pool))) {
      LOG_WARN("fail to assign new pool", K(ret));
    } else if (OB_FAIL(new_pool.zone_list_.assign(new_zone_list))) {
      LOG_WARN("fail to assign new pool zone list", K(ret));
    } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
      LOG_WARN("start transaction failed", K(ret));
    } else {
      if (OB_FAIL(ut_operator_.remove_units_in_zones(
              trans, pool->resource_pool_id_, to_be_removed_zones))) {
        LOG_WARN("fail to remove units in zones", K(ret),
                 "pool id", pool->resource_pool_id_, K(to_be_removed_zones));
      } else if (OB_FAIL(ut_operator_.update_resource_pool(trans, new_pool))) {
        LOG_WARN("fail to update resource pool", K(ret));
      } else {} // no more to do
      if (trans.is_started()) {
        const bool commit = (OB_SUCC(ret));
        int tmp_ret = OB_SUCCESS;
        if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
          LOG_WARN("fail to end trans", K(commit), K(tmp_ret));
          ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(delete_units_in_zones(pool->resource_pool_id_, to_be_removed_zones))) {
        LOG_WARN("fail to delete units in zones", K(ret),
                 "pool id", pool->resource_pool_id_, K(to_be_removed_zones));
      } else if (OB_FAIL(pool->zone_list_.assign(new_zone_list))) {
        LOG_WARN("fail to update pool zone list in memory", K(ret));
      } else {} // no more to do
    }
  }
  return ret;
}

int ObUnitManager::cal_to_be_removed_pool_zone_list(
    const common::ObIArray<common::ObZone> &prev_zone_list,
    const common::ObIArray<common::ObZone> &cur_zone_list,
    common::ObIArray<common::ObZone> &to_be_removed_zones) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(prev_zone_list.count() <= 0 || cur_zone_list.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret),
             "prev zone list", prev_zone_list,
             "cur zone list", cur_zone_list);
  } else {
    to_be_removed_zones.reset();
    // Each zone in cur_zone_list must be included in prev_zone_list
    for (int64_t i = 0; OB_SUCC(ret) && i < cur_zone_list.count(); ++i) {
      const common::ObZone &this_zone = cur_zone_list.at(i);
      if (!has_exist_in_array(prev_zone_list, this_zone)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "alter resource pool zone list with a new zone");
        LOG_WARN("alter resource pool zone list with a new zone is not supported", K(ret));
      } else {} // good
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < prev_zone_list.count(); ++i) {
      const common::ObZone &this_zone = prev_zone_list.at(i);
      if (has_exist_in_array(cur_zone_list, this_zone)) {
        // still exist, do nothing
      } else if (OB_FAIL(to_be_removed_zones.push_back(this_zone))) {
        LOG_WARN("fail to push back", K(ret));
      } else {} // no more to do
    }
  }
  return ret;
}

int ObUnitManager::check_full_resource_pool_memory_condition(
    const common::ObIArray<share::ObResourcePool *> &pools,
    const int64_t memory_size) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
    const share::ObResourcePool *pool = pools.at(i);
    if (OB_UNLIKELY(nullptr == pool)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool ptr is null", K(ret), KP(pool));
    } else if (REPLICA_TYPE_FULL != pool->replica_type_) {
      // bypass
    } else if (memory_size < GCONF.__min_full_resource_pool_memory) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("full resource pool min memory illegal", KR(ret), K(memory_size),
          "__min_full_resource_pool_memory", (int64_t)GCONF.__min_full_resource_pool_memory);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unit MEMORY_SIZE less than __min_full_resource_pool_memory");
    }
  }
  return ret;
}

int ObUnitManager::check_expand_resource_(
    const char *module,
    const common::ObIArray<share::ObResourcePool  *> &pools,
    const share::ObUnitResource &old_resource,
    const share::ObUnitResource &new_resource) const
{
  int ret = OB_SUCCESS;
  common::hash::ObHashMap<ObAddr, int64_t> server_ref_count_map;
  common::ObZone zone;
  ObString err_str;
  AlterResourceErr err_index = ALT_ERR;
  int temp_ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (pools.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pools is empty", K(pools), K(ret));
  } else if (!old_resource.is_valid() || !new_resource.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid old_resource or invalid new_resource", K(old_resource), K(new_resource), K(ret));
  } else if (OB_FAIL(server_ref_count_map.create(
      SERVER_REF_COUNT_MAP_BUCKET_NUM, ObModIds::OB_HASH_BUCKET_SERVER_REF_COUNT_MAP))) {
    LOG_WARN("pool_unit_map_ create failed",
        "bucket_num", static_cast<int64_t>(SERVER_REF_COUNT_MAP_BUCKET_NUM), K(ret));
  } else if (OB_FAIL(get_pools_servers(pools, server_ref_count_map))) {
    LOG_WARN("get pools server failed", K(pools), K(ret));
  } else {
    bool can_expand = true;
    const ObUnitResource delta = new_resource - old_resource;
    ObUnitResource expand_resource;

    _LOG_INFO("[%s] check_expand_resource begin. old=%s, new=%s, delta=%s", module,
        to_cstring(old_resource), to_cstring(new_resource), to_cstring(delta));

    FOREACH_X(iter, server_ref_count_map, OB_SUCCESS == ret) {
      expand_resource = delta * (iter->second);

      _LOG_INFO("[%s] check_expand_resource. svr=%s, pools=%ld, expand_resource=%s", module,
          to_cstring(iter->first), iter->second, to_cstring(expand_resource));

      if (OB_FAIL(check_expand_resource_(iter->first, expand_resource, can_expand, err_index))) {
        LOG_WARN("check expand resource failed", K(ret));
      } else if (!can_expand) {
        if (OB_FAIL(server_mgr_.get_server_zone(iter->first, zone))) {
          LOG_WARN("get_server_zone failed", K(iter->first), K(ret));
        } else {
          LOG_USER_ERROR(OB_MACHINE_RESOURCE_NOT_ENOUGH, to_cstring(zone), to_cstring(iter->first),
              alter_resource_err_to_str(err_index));

          // return ERROR
          ret = OB_MACHINE_RESOURCE_NOT_ENOUGH;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_expand_resource_(const ObAddr &server,
    const ObUnitResource &expand_resource, bool &can_expand, AlterResourceErr &err_index) const
{
  int ret = OB_SUCCESS;
  ObServerStatus status;
  double hard_limit = 0;
  bool can_hold_unit = false;
  can_expand = true;
  // some item of expand_resource may be negative, so we don't check expand_resource here
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server", K(server), K(ret));
  } else if (OB_FAIL(get_hard_limit(hard_limit))) {
    LOG_WARN("get_hard_limit failed", K(ret));
  } else if (OB_FAIL(server_mgr_.get_server_status(server, status))) {
    LOG_WARN("get_server_status failed", K(server), K(ret));
  } else if (OB_FAIL(have_enough_resource(status, expand_resource, hard_limit, can_hold_unit, err_index))) {
    LOG_WARN("fail to check have enough resource", K(status), K(hard_limit), K(ret));
  } else if (!can_hold_unit) {
    can_expand = false;
    // don't need to set ret
    LOG_WARN("find server can't hold expanded resource", K(server), K(status), K(expand_resource));
  } else {
    can_expand = true;
  }
  return ret;
}

int ObUnitManager::check_shrink_resource_(const ObIArray<share::ObResourcePool *> &pools,
                                          const ObUnitResource &resource,
                                          const ObUnitResource &new_resource) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (pools.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pools is empty", K(pools), K(ret));
  } else if (!resource.is_valid() || !new_resource.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid resource or invalid new_resource", K(resource), K(new_resource), K(ret));
  } else {
    if (new_resource.max_cpu() < resource.max_cpu()) {
      // cpu don't need check
    }

    if (new_resource.memory_size() < resource.memory_size()
        || new_resource.log_disk_size() < resource.log_disk_size()) {
      for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
        const share::ObResourcePool *pool = pools.at(i);
        if (OB_UNLIKELY(NULL == pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool ptr is null", K(ret));
        } else if (OB_FAIL(check_shrink_resource_(*pool, resource, new_resource))) {
          LOG_WARN("fail to check shrink resource", KR(ret));
        } else {} // no more to do
      }
    }
  }
  return ret;
}

int ObUnitManager::check_shrink_resource_(const share::ObResourcePool &pool,
                                          const ObUnitResource &resource,
                                          const ObUnitResource &new_resource) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!pool.is_valid() || !resource.is_valid() || !new_resource.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid pool or invalid resource or invalid new_resource", K(pool),
        K(resource), K(new_resource), K(ret));
  } else {
    if (new_resource.max_cpu() < resource.max_cpu()) {
      // cpu don't need check
    }

    if (new_resource.memory_size() < resource.memory_size()) {
      if (!pool.is_granted_to_tenant()) {
        // do nothing
      } else if (OB_FAIL(check_shrink_memory(pool,resource.memory_size(), new_resource.memory_size()))) {
        LOG_WARN("check_shrink_memory failed", "tenant_id", pool.tenant_id_,
            "old_resource_mem", resource.memory_size(),
            "new_resource_mem", new_resource.memory_size(), KR(ret));
      }
    }

    if (new_resource.log_disk_size() < resource.log_disk_size()) {
      // log disk don't need check.
    } 
  }
  return ret;
}

int ObUnitManager::check_shrink_memory(
    const share::ObResourcePool &pool,
    const int64_t old_memory,
    const int64_t new_memory) const
{
  int ret = OB_SUCCESS;
  ObArray<ObUnit *> *units = NULL;
  ObArray<common::ObAddr> unit_servers;

  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (! pool.is_granted_to_tenant()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool is not granted to valid tenant, invalid pool", KR(ret), K(pool));
  } else if (old_memory <= 0 || new_memory <= 0 || new_memory >= old_memory) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid old_memory or invalid new_memory",
        K(old_memory), K(new_memory), K(ret));
  } else if (OB_FAIL(get_units_by_pool(pool.resource_pool_id_, units))) {
    LOG_WARN("fail to get units by pool", K(ret));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units ptr is null", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret));
      } else if (OB_FAIL(unit_servers.push_back(unit->server_))) {
        LOG_WARN("fail to push back", K(ret));
      } else if (!unit->migrate_from_server_.is_valid()) {
        // this unit is not in migrating
      } else if (OB_FAIL(unit_servers.push_back(unit->migrate_from_server_))) {
        LOG_WARN("fail to push back", K(ret));
      }
    }
    const double max_used_ratio = 0.9;
    const double shrink_ratio = static_cast<double>(new_memory)
        / static_cast<double>(old_memory);
    int64_t max_used_memory = 0;
    ObArray<ObTenantMemstoreInfoOperator::TenantServerMemInfo> mem_infos;
    ObTenantMemstoreInfoOperator mem_info_operator(*proxy_);
    if (OB_FAIL(ret)) {
      // failed
    } else if (OB_FAIL(mem_info_operator.get(pool.tenant_id_, unit_servers, mem_infos))) {
      LOG_WARN("mem_info_operator get failed", K(ret));
    } else {
      FOREACH_CNT_X(mem_info, mem_infos, OB_SUCCESS == ret) {
        max_used_memory = static_cast<int64_t>(static_cast<double>(mem_info->memstore_limit_)
            * shrink_ratio * max_used_ratio);
        if (mem_info->total_memstore_used_ > max_used_memory) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "requested memory over 90 percent of total available memory");
          LOG_WARN("new memory will cause memory use percentage over ninety percentage",
              "mem_info", *mem_info, K(old_memory), K(new_memory),
              K(max_used_ratio), K(max_used_memory), K(ret));
        }
      }
    }
  }
  return ret;
}

// server count of pool won't be large, so two loop is ok
int ObUnitManager::check_duplicated_server(const ObIArray<ObAddr> &servers,
                                           bool &duplicated_exist)
{
  int ret = OB_SUCCESS;
  duplicated_exist = false;
  for (int64_t i = 0; i < servers.count(); ++i) {
    for (int64_t j = i + 1; j < servers.count(); ++j) {
      if (servers.at(i) == servers.at(j)) {
        duplicated_exist = true;
      }
    }
  }
  return ret;
}

int ObUnitManager::change_pool_config(share::ObResourcePool *pool, ObUnitConfig *config,
                                      ObUnitConfig *new_config)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (NULL == pool || NULL == config || NULL == new_config) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KP(pool), KP(config), KP(new_config), K(ret));
  } else {
    share::ObResourcePool new_pool;
    if (OB_FAIL(new_pool.assign(*pool))) {
      LOG_WARN("failed to assign new_pool", K(ret));
    }
    if (OB_SUCC(ret)) {
      new_pool.unit_config_id_ = new_config->unit_config_id();
      if (OB_FAIL(ut_operator_.update_resource_pool(*proxy_, new_pool))) {
        LOG_WARN("ut_operator_ update_resource_pool failed", K(new_pool), K(ret));
      } else if (OB_FAIL(dec_config_ref_count(config->unit_config_id()))) {
        LOG_WARN("dec_config_ref_count failed", "unit_config_id",
            config->unit_config_id(), K(ret));
      } else if (OB_FAIL(inc_config_ref_count(new_config->unit_config_id()))) {
        LOG_WARN("inc_config_ref_count failed", "unit_config_id",
            new_config->unit_config_id(), K(ret));
      } else if (OB_FAIL(delete_config_pool(config->unit_config_id(), pool))) {
        LOG_WARN("delete config pool failed", "config id", config->unit_config_id(), K(ret));
      } else if (OB_FAIL(insert_config_pool(new_config->unit_config_id(), pool))) {
        LOG_WARN("insert config pool failed", "config id", new_config->unit_config_id(), K(ret));
      } else if (OB_FAIL(update_pool_load(pool, new_config))) {
        LOG_WARN("update resource pool load failed", K(ret), "resource_pool", *pool,
            "unit_config", *new_config);
      } else {
        pool->unit_config_id_ = new_config->unit_config_id();
      }
    }
  }
  return ret;
}

// The zones of multiple pools have no intersection
// 14x new semantics. If it is the source_pool used to store the copy of L, it can be compared
int ObUnitManager::check_pool_intersect(
    const uint64_t tenant_id,
    const ObIArray<ObResourcePoolName> &pool_names,
    bool &intersect)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, OB_DEFAULT_REPLICA_NUM> zones;
  common::ObArray<share::ObResourcePool *> *pools = NULL;;
  intersect = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (pool_names.count() <= 0 || !is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool_names is empty", K(pool_names), K(tenant_id), K(ret));
  } else {
    FOREACH_CNT_X(pool_name, pool_names, OB_SUCCESS == ret && !intersect) {
      share::ObResourcePool *pool = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(*pool_name, pool))) {
        LOG_WARN("get resource pool by name failed", "pool_name", *pool_name, K(ret));
      } else if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", KP(pool), K(ret));
      } else {
        FOREACH_CNT_X(zone, pool->zone_list_, OB_SUCCESS == ret && !intersect) {
          if (NULL == zone) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit is null", K(ret));
          } else {
            ObString zone_str;
            zone_str.assign_ptr(zone->ptr(), static_cast<int32_t>(zone->size()));
            if (has_exist_in_array(zones, zone_str)) {
              intersect = true;
            } else if (OB_FAIL(zones.push_back(zone_str))) {
              LOG_WARN("push_back failed", K(ret));
            }
          }
        }  // end foreach zone
      }
    }  // end foreach pool
    if (OB_FAIL(ret)) {
    } else if (intersect) {
    } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // a new tenant, without resource pool already granted
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
      }
    } else if (OB_UNLIKELY(NULL == pools)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", K(ret), KP(pools));
    } else {
      for (int64_t i = 0; !intersect && OB_SUCC(ret) && i < pools->count(); ++i) {
        const share::ObResourcePool *pool = pools->at(i);
        if (OB_UNLIKELY(NULL == pool)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("pool ptr is null", K(ret), KP(pool));
        } else {
          for (int64_t j = 0; !intersect && OB_SUCC(ret) && j < zones.count(); ++j) {
            common::ObZone zone;
            if (OB_FAIL(zone.assign(zones.at(j).ptr()))) {
              LOG_WARN("fail to assign zone", K(ret));
            } else if (has_exist_in_array(pool->zone_list_, zone)) {
              intersect = true;
            } else {} // good
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::change_pool_owner(
    ObISQLClient &client,
    const common::ObIArray<uint64_t> &new_ug_ids,
    const lib::Worker::CompatMode compat_mode,
    const bool grant,
    const ObIArray<ObResourcePoolName> &pool_names,
    const uint64_t tenant_id,
    const bool is_bootstrap,
    const bool if_not_grant,
    const bool skip_offline_server)
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!is_valid_tenant_id(tenant_id) || pool_names.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(pool_names), K(ret));
  } else if (OB_UNLIKELY(nullptr == srv_rpc_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("srv_rpc_proxy_ ptr is null", K(ret));
  } else {
    const bool is_delete_server_resource = !grant;
    const uint64_t new_tenant_id = (grant ? tenant_id : OB_INVALID_ID);
    ObNotifyTenantServerResourceProxy notify_proxy(
                                      *srv_rpc_proxy_,
                                      &obrpc::ObSrvRpcProxy::notify_tenant_server_unit_resource);
    share::ObResourcePool new_pool;
    for (int64_t i = 0; OB_SUCC(ret) && i < pool_names.count(); ++i) {
      share::ObResourcePool *pool = NULL;
      ObArray<ObUnit *> *units = nullptr;
      common::ObArray<ObUnit *> zone_sorted_unit_array;
      if (OB_FAIL(inner_get_resource_pool_by_name(pool_names.at(i), pool))) {
        LOG_WARN("get resource pool by name failed", "pool_name", pool_names.at(i), K(ret));
      } else if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", KP(pool), K(ret));
      } else if (grant && pool->is_granted_to_tenant()) {
        ret = OB_RESOURCE_POOL_ALREADY_GRANTED;
        LOG_USER_ERROR(OB_RESOURCE_POOL_ALREADY_GRANTED, to_cstring(pool_names.at(i)));
        LOG_WARN("pool has already granted to other tenant, can't grant again",
                 K(ret), K(tenant_id), "pool", *pool);
      } else if (!grant && pool->is_granted_to_tenant() && pool->tenant_id_ != tenant_id) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("find pool already granted to other tenant, can not revoke",
            "pool", *pool, K(tenant_id), K(ret));
      } else if (!grant && ! pool->is_granted_to_tenant()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("find pool not granted to any tenant, can not revoke",
            "pool", *pool, K(tenant_id), K(ret));
      } else if (OB_FAIL(new_pool.assign(*pool))) {
        LOG_WARN("failed to assign new_pool", K(ret));
      } else if (FALSE_IT(new_pool.tenant_id_ = new_tenant_id)) {
        // shall never be here
      } else if (OB_FAIL(ut_operator_.update_resource_pool(client, new_pool))) {
        LOG_WARN("update_resource_pool failed", K(new_pool), K(ret));
      } else if (OB_FAIL(get_units_by_pool(new_pool.resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", K(ret));
      } else if (OB_UNLIKELY(nullptr == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptr is null", K(ret));
      } else if (is_bootstrap) {
        //no need to notify unit and modify unit group id
      } else if (pool->unit_count_ != new_ug_ids.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("count not match", KR(ret), K(tenant_id),
                 "pool", new_pool, K(new_ug_ids));
      } else if (OB_FAIL(zone_sorted_unit_array.assign(*units))) {
        LOG_WARN("fail to assign zone unit array", KR(ret));
      } else {
        UnitZoneOrderCmp cmp_operator;
        std::sort(zone_sorted_unit_array.begin(), zone_sorted_unit_array.end(), cmp_operator);
        ObUnit new_unit;
        for (int64_t j = 0; OB_SUCC(ret) && j < zone_sorted_unit_array.count(); ++j) {
          ObUnit *unit = zone_sorted_unit_array.at(j);
          new_unit.reset();
          if (OB_UNLIKELY(nullptr == unit)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", KR(ret));
          } else if (OB_FAIL(try_notify_tenant_server_unit_resource(
                  tenant_id, is_delete_server_resource, notify_proxy,
                  new_pool, compat_mode, *unit, if_not_grant, skip_offline_server))) {
            LOG_WARN("fail to try notify server unit resource", K(ret));
          } else if (FALSE_IT(new_unit = *unit)) {
            // shall never be here
          } else if (FALSE_IT(new_unit.unit_group_id_ = new_ug_ids.at(j % (pool->unit_count_)))) {
            // shall never be here
          } else if (OB_FAIL(ut_operator_.update_unit(client, new_unit))) {
            LOG_WARN("fail to update unit", KR(ret));
          }
        }
      }
    }
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = notify_proxy.wait())) {
      LOG_WARN("fail to wait notify resource", K(ret), K(tmp_ret));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

int ObUnitManager::get_zone_units(const ObArray<share::ObResourcePool *> &pools,
                                  ObArray<ZoneUnit> &zone_units) const
{
  int ret = OB_SUCCESS;
  ObArray<ObZone> zones;
  zone_units.reuse();
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K_(inited), K_(loaded), K(ret));
  } else if (pools.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pools is empty", K(pools), K(ret));
  } else {
    FOREACH_CNT_X(pool, pools, OB_SUCCESS == ret) {
      if (NULL == *pool) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("pool is null", "pool", OB_P(*pool), K(ret));
      } else {
        FOREACH_CNT_X(pool_zone, (*pool)->zone_list_, OB_SUCCESS == ret) {
          bool find = false;
          FOREACH_CNT_X(zone, zones, !find) {
            if (*zone == *pool_zone) {
              find = true;
            }
          }
          if (!find) {
            if (OB_FAIL(zones.push_back(*pool_zone))) {
              LOG_WARN("push_back failed", K(ret));
            }
          }
        }
      }
    }

    ZoneUnit zone_unit;
    ObArray<ObUnitInfo> unit_infos;
    FOREACH_CNT_X(zone, zones, OB_SUCCESS == ret) {
      zone_unit.reset();
      zone_unit.zone_ = *zone;
      FOREACH_CNT_X(pool, pools, OB_SUCCESS == ret) {
        unit_infos.reuse();
        if (NULL == *pool) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("pool is null", "pool", OB_P(*pool), K(ret));
        } else if (OB_FAIL(inner_get_unit_infos_of_pool((*pool)->resource_pool_id_, unit_infos))) {
          LOG_WARN("inner_get_unit_infos_of_pool failed",
              "pool id", (*pool)->resource_pool_id_, K(ret));
        } else {
          FOREACH_CNT_X(unit_info, unit_infos, OB_SUCCESS == ret) {
            if (unit_info->unit_.zone_ == *zone) {
              if (OB_FAIL(zone_unit.unit_infos_.push_back(*unit_info))) {
                LOG_WARN("push_back failed", K(ret));
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(zone_units.push_back(zone_unit))) {
          LOG_WARN("push_back failed", K(zone_unit), K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_tenants_of_server(const common::ObAddr &server,
    common::hash::ObHashSet<uint64_t> &tenant_id_set) const
{
  int ret = OB_SUCCESS;
  ObArray<ObUnitLoad> *unit_loads = NULL;
  {
    SpinRLockGuard guard(lock_);
    if (!check_inner_stat()) {
      ret = OB_INNER_STAT_ERROR;
      LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
    } else if (!server.is_valid()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("server is invalid", K(server), K(ret));
    } else if (OB_FAIL(get_loads_by_server(server, unit_loads))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get_loads_by_server failed", K(server), K(ret));
      } else {
        ret = OB_SUCCESS;
        // just return empty set
      }
    } else if (NULL == unit_loads) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit_loads is null", KP(unit_loads), K(ret));
    }
    if (OB_SUCC(ret) && !OB_ISNULL(unit_loads)) {
      FOREACH_CNT_X(unit_load, *unit_loads, OB_SUCCESS == ret) {
        if (!unit_load->is_valid()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid unit_load", "unit_load", *unit_load, K(ret));
        } else {
          const uint64_t tenant_id = unit_load->pool_->tenant_id_;
          if (!is_valid_tenant_id(tenant_id)) {
            //do nothing
          } else if (OB_FAIL(tenant_id_set.set_refactored(tenant_id))) {
            if (OB_HASH_EXIST == ret) {
              ret = OB_SUCCESS;
            } else {
              LOG_WARN("set tenant id failed", K(tenant_id), K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_tenant_on_server(const uint64_t tenant_id,
    const ObAddr &server, bool &on_server) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!is_valid_tenant_id(tenant_id) || !server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid tenant_id or invalid server", K(tenant_id), K(server), K(ret));
  } else {
    ObArray<uint64_t> pool_ids;
    ObZone zone;
    ObArray<ObAddr> servers;
    if (OB_FAIL(get_pool_ids_of_tenant(tenant_id, pool_ids))) {
      LOG_WARN("get_pool_ids_of_tenant failed", K(tenant_id), K(ret));
    } else if (OB_FAIL(server_mgr_.get_server_zone(server, zone))) {
      LOG_WARN("get_server_zone failed", K(server), K(ret));
    } else {
      SpinRLockGuard guard(lock_);
      FOREACH_CNT_X(pool_id, pool_ids, OB_SUCCESS == ret && !on_server) {
        if (OB_FAIL(get_pool_servers(*pool_id, zone, servers))) {
          LOG_WARN("get_pool_servers failed", "pool_id", *pool_id, K(zone), K(ret));
        } else if (has_exist_in_array(servers, server)) {
          on_server = true;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::admin_migrate_unit(
    const uint64_t unit_id,
    const ObAddr &dst,
    bool is_cancel)
{
  int ret = OB_SUCCESS;
  ObUnitInfo unit_info;
  ObArray<ObAddr> excluded_servers;
  ObServerStatus status;
  ObUnitConfig left_resource;
  ObZone src_zone;
  ObZone dst_zone;
  double hard_limit = 0;
  bool can_hold_unit = false;
  bool is_manual = true;
  const bool new_allocate_pool = false;
  SpinWLockGuard guard(lock_);
  AlterResourceErr err_index = ALT_ERR;
  const char *module = "ADMIN_MIGRATE_UNIT";
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), KR(ret));
  } else if (OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit id is invalid", K(unit_id), KR(ret));
  } else if (!dst.is_valid() && !is_cancel) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("dst is invalid", K(dst), KR(ret));
  } else if (OB_FAIL(get_hard_limit(hard_limit))) {
    LOG_WARN("get_hard_limit failed", KR(ret));
  } else if (OB_FAIL(inner_get_unit_info_by_id(unit_id, unit_info))) {
    LOG_WARN("get unit info failed", K(unit_id), KR(ret));
  } else if (ObUnit::UNIT_STATUS_ACTIVE != unit_info.unit_.status_) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("migrate a unit which is in deleting status", KR(ret), K(unit_id));
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, "migrate a unit which is in deleting status");
  } else if (OB_FAIL(server_mgr_.get_server_zone(unit_info.unit_.server_, src_zone))) {
    LOG_WARN("get server zone failed", "server", unit_info.unit_.server_, KR(ret));
  } else if (dst == unit_info.unit_.migrate_from_server_ || is_cancel) {
    // cancel migrate unit
    if (is_cancel && !unit_info.unit_.migrate_from_server_.is_valid()) {
	    ret = OB_ERR_UNEXPECTED;
	    LOG_WARN("failed to cancel migrate unit, may be no migrate task", KR(ret), K(unit_info));
      LOG_USER_ERROR(OB_ERR_UNEXPECTED,"no migrate task to cancel");
	  } else if (OB_FAIL(cancel_migrate_unit(
            unit_info.unit_, unit_info.pool_.tenant_id_ == OB_GTS_TENANT_ID))) {
		  LOG_WARN("failed to cancel migrate unit", KR(ret), K(unit_info));
    }
  } else if (OB_FAIL(server_mgr_.get_server_zone(dst, dst_zone))) {
    LOG_WARN("get server zone failed", "server", dst, KR(ret));
  } else if (src_zone != dst_zone) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("migrate unit between zones is not supported", KR(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED,"migrate unit between zones");
  } else if (OB_FAIL(get_excluded_servers(unit_info.unit_.resource_pool_id_, unit_info.unit_.zone_,
      module, new_allocate_pool, excluded_servers))) {
    LOG_WARN("get_excluded_servers failed", "unit", unit_info.unit_, KR(ret), K(new_allocate_pool));
  } else if (has_exist_in_array(excluded_servers, dst)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED,"hold two units of a tenant in the same server");
    LOG_WARN("hold two units of a tenant in the same server is not supported", KR(ret));
  } else if (OB_FAIL(server_mgr_.get_server_status(dst, status))) {
    LOG_WARN("get server status failed", "server", dst, KR(ret));
  } else if (!status.can_migrate_in()) {
    ret = OB_SERVER_MIGRATE_IN_DENIED;
    LOG_WARN("server can not migrate in", K(dst), K(status), KR(ret));
  } else if (OB_FAIL(have_enough_resource(status, unit_info.config_.unit_resource(), hard_limit, can_hold_unit, err_index))) {
      LOG_WARN("calculate_left_resource failed", "status", status, K(hard_limit), K(err_index), KR(ret));
  } else if (!can_hold_unit) {
    ret = OB_MACHINE_RESOURCE_NOT_ENOUGH;
    LOG_WARN("left resource can't hold unit", "server", dst,
        K(hard_limit), K(left_resource), "config", unit_info.config_, KR(ret));
  } else if (OB_FAIL(migrate_unit(unit_id, dst, is_manual))) {
    LOG_WARN("migrate unit failed", K(unit_id), "destination", dst, KR(ret));
  }

  return ret;
}

int ObUnitManager::cancel_migrate_unit(
    const share::ObUnit &unit,
    const bool is_gts_unit)
{
  int ret = OB_SUCCESS;
  ObServerStatus status;
  if (OB_FAIL(server_mgr_.get_server_status(unit.migrate_from_server_, status))) {
    LOG_WARN("get_server_status failed", "server", unit.migrate_from_server_, K(ret));
  } else if (!status.can_migrate_in() && !is_gts_unit) {
    ret = OB_SERVER_MIGRATE_IN_DENIED;
    LOG_WARN("server can not migrate in", "server", unit.migrate_from_server_, K(status), K(ret));
  } else {
    const EndMigrateOp op = REVERSE;
    if (OB_FAIL(end_migrate_unit(unit.unit_id_, op))) {
      LOG_WARN("end_migrate_unit failed", "unit_id", unit.unit_id_, K(op), K(ret));
    } else {
      LOG_INFO("cancel migrate unit", K(unit));
    }
  }
  return ret;
}

int ObUnitManager::try_cancel_migrate_unit(const share::ObUnit &unit, bool &is_canceled)
{
  int ret = OB_SUCCESS;
  ObServerStatus status;
  bool can_migrate_in = false;
  is_canceled = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!unit.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit", K(unit), K(ret));
  } else if (OB_FAIL(check_can_migrate_in(unit.server_, can_migrate_in))) {
    LOG_WARN("check_can_migrate_in failed", "server", unit.server_, K(ret));
  } else if (can_migrate_in) {
    // ignore, do nothing
  } else if (!unit.migrate_from_server_.is_valid()) {
    // ignore, do nothing
  } else if (OB_FAIL(server_mgr_.get_server_status(unit.migrate_from_server_, status))) {
    LOG_WARN("get_server_status failed", "server", unit.migrate_from_server_, K(ret));
  } else if (status.can_migrate_in()) {
    LOG_INFO("unit migrate_from_server can migrate in, "
        "migrate unit back to migrate_from_server", K(unit), K(status));
    const EndMigrateOp op = REVERSE;
    if (OB_FAIL(end_migrate_unit(unit.unit_id_, op))) {
      LOG_WARN("end_migrate_unit failed", "unit_id", unit.unit_id_, K(op), K(ret));
    } else {
      is_canceled = true;
      LOG_INFO("reverse unit migrate success", K(ret), "unit_id", unit.unit_id_, K(op));
    }
  }
  return ret;
}

int ObUnitManager::get_server_loads_internal(const ObZone &zone,
                                             const bool only_active,
                                             ObArray<ObServerLoad> &server_loads,
                                             double &sum_load,
                                             int64_t &alive_server_count,
                                             double *weights, int64_t weights_count)
{
  int ret = OB_SUCCESS;
  ObServerManager::ObServerStatusArray server_statuses;
  // zone can be empty, don't check it
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else if (OB_FAIL(server_mgr_.get_server_statuses(zone, server_statuses))) {
    LOG_WARN("get_servers_of_zone failed", K(zone), K(ret));
  } else {
    alive_server_count = 0;
    sum_load = 0;
  }

  ObServerLoad server_load;
  for (int64_t i = 0; OB_SUCC(ret) && i < server_statuses.count(); ++i) {
    ObArray<ObUnitLoad> *unit_loads = NULL;
    ObServerStatus &status = server_statuses.at(i);
    server_load.reset();
    if (only_active && !status.is_active()) {
      // filter not active server
    } else {
      if (status.is_active()) {
        ++alive_server_count;
      }
      if (OB_FAIL(get_loads_by_server(status.server_, unit_loads))) {
        if (OB_ENTRY_NOT_EXIST != ret) {
          LOG_WARN("get_loads_by_server failed", "server", status.server_, K(ret));
        } else {
          ret = OB_SUCCESS;
          LOG_DEBUG("server is empty, no unit on it", "server", status.server_);
        }
      } else if (NULL == unit_loads) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit_loads is null", KP(unit_loads), K(ret));
      }

      // unit_loads is null if no unit on it
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(server_load.build(unit_loads, status))) {
        LOG_WARN("server_load build failed", K(status), K(ret));
      } else if (OB_FAIL(server_loads.push_back(server_load))) {
        LOG_WARN("push_back failed", K(ret));
      } else {}
    }
  }  // end for
  if (OB_SUCC(ret) && server_loads.count() > 0) {
    if (OB_FAIL(ObResourceUtils::calc_server_resource_weight(server_loads, weights, weights_count))) {
      LOG_WARN("failed to calc resource weight", K(ret));
    } else {
      double load = 0;
      ARRAY_FOREACH(server_loads, i) {
        ObServerLoad &server_load = server_loads.at(i);
        if (OB_FAIL(server_load.get_load(weights, weights_count, load))) {
          LOG_WARN("get_load_percentage failed", K(ret));
        } else {
          sum_load += load;
        }
      }  // end for
    }
  }
  return ret;
}

int ObUnitManager::get_server_loads(const ObZone &zone,
                                    ObArray<ObServerLoad> &server_loads,
                                    double *weights, int64_t weights_count)
{
  int ret = OB_SUCCESS;
  double sum_load = 0;
  int64_t alive_server_count = 0;
  const bool only_active = false;
  SpinRLockGuard guard(lock_);
  // zone can be empty, don't check it
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else if (OB_FAIL(get_server_loads_internal(zone, only_active,
                                               server_loads, sum_load, alive_server_count, weights, weights_count))) {
    LOG_WARN("fail to get server loads internal", K(zone), K(only_active), K(ret));
  }

  return ret;
}

int ObUnitManager::get_hard_limit(double &hard_limit) const
{
  int ret = OB_SUCCESS;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else {
    hard_limit = static_cast<double>(server_config_->resource_hard_limit) / 100;
  }
  return ret;
}

// Check if the unit in the unit group has a migration task in progress
int ObUnitManager::check_unit_group_normal(const ObUnit &unit, bool &normal)
{
  int ret = OB_SUCCESS;
  normal = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!unit.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit", K(unit), K(ret));
  } else {
    normal = unit.server_.is_valid()
             && !unit.migrate_from_server_.is_valid()
             && ObUnit::UNIT_STATUS_ACTIVE == unit.status_;
    if (normal) {
      ObArray<ObUnit *> *units = NULL;
      if (OB_FAIL(get_units_by_pool(unit.resource_pool_id_, units))) {
        LOG_WARN("get units by pool failed",
            K(ret), "resource_pool_id", unit.resource_pool_id_);
      } else if (NULL == units) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL units returned", K(ret));
      } else {
        FOREACH_X(u, *units, OB_SUCC(ret) && normal) {
          if (NULL == *u) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("NULL unit", K(ret));
          } else {
            // Check whether the partition in u has an intersection with the partition in unit
            // If there is an intersection and u is being migrated, normal = false, don't migrate unit for now
            //
            // Current strategy: As long as one unit in the zone is migrating, the remaining units cannot be migrated
            bool intersect = false;
            if (OB_SUCC(check_has_intersect_pg(unit, **u, intersect))) {
              if (intersect
                  && ((*u)->migrate_from_server_.is_valid()
                      || ObUnit::UNIT_STATUS_ACTIVE != (*u)->status_)) {
                normal = false;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::check_has_intersect_pg(const share::ObUnit &a,
                                          const share::ObUnit &b,
                                          bool &intersect)
{
  UNUSED(a);
  UNUSED(b);
  intersect = true;
  return OB_SUCCESS;
}

int ObUnitManager::check_can_migrate_in(const ObAddr &server, bool &can_migrate_in) const
{
  int ret = OB_SUCCESS;
  ObServerStatus status;
  can_migrate_in = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check inner stat failed", K_(inited), K_(loaded), K(ret));
  } else if (!server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server", K(server), K(ret));
  } else if (OB_FAIL(server_mgr_.get_server_status(server, status))) {
    LOG_WARN("get_server_status failed", K(server), K(ret));
  } else {
    can_migrate_in = status.can_migrate_in();
  }
  return ret;
}

int ObUnitManager::try_migrate_unit(const uint64_t unit_id,
                                    const uint64_t tenant_id,
                                    const ObUnitStat &unit_stat,
                                    const ObIArray<ObUnitStat> &migrating_unit_stat,
                                    const ObAddr &dst,
                                    const bool is_manual)
{
  int ret = OB_SUCCESS;
  ObServerStatus server_status;
  if (unit_id != unit_stat.unit_id_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit stat", K(unit_id), K(unit_stat), K(ret));
  } else if (OB_FAIL(server_mgr_.get_server_status(dst, server_status))) {
    LOG_WARN("fail get server status", K(dst), K(ret));
  } else {
    int64_t mig_partition_cnt = 0;
    int64_t mig_required_size = 0;
    for (int64_t i = 0; i < migrating_unit_stat.count(); ++i) {
      mig_partition_cnt += migrating_unit_stat.at(i).partition_cnt_;
      mig_required_size +=  migrating_unit_stat.at(i).required_size_;
    }
    //// partition number list
    //int64_t required_cnt =
    //    mig_partition_cnt + unit_stat.partition_cnt_ + server_status.resource_info_.partition_cnt_;
    // sstable Space constraints
    int64_t required_size =
        mig_required_size + unit_stat.required_size_ + server_status.resource_info_.disk_in_use_;
    int64_t total_size = server_status.resource_info_.disk_total_;
    int64_t required_percent = (100 * required_size) / total_size;
    int64_t limit_percent = GCONF.data_disk_usage_limit_percentage;
// 4.0 does not restrict OB_MAX_PARTITION_NUM_PER_SERVER
//
//    if (server_status.resource_info_.partition_cnt_ < 0) {
//      // The old version of the server cannot get the count and size information, it is always allowed to move in
//    if ( required_cnt > OB_MAX_PARTITION_NUM_PER_SERVER) {
//      ret = OB_OP_NOT_ALLOW;
//      LOG_ERROR("migrate unit fail. dest server has too many partitions",
//                K(unit_id), K(unit_stat), K(dst),
//                K(required_cnt), "limit_cnt", OB_MAX_PARTITION_NUM_PER_SERVER, K(ret));
    if (required_percent >= limit_percent) {
      ret = OB_OP_NOT_ALLOW;
      LOG_ERROR("migrate unit fail. dest server out of space",
                K(unit_id), K(unit_stat), K(dst),
                K(required_size), K(total_size), K(limit_percent), K(ret));
    }

    if (FAILEDx(migrate_unit(unit_id, dst, is_manual))) {
      LOG_WARN("fail migrate unit", K(unit_id), K(dst), K(ret));
    }
  }
  return ret;
}

int ObUnitManager::migrate_unit(const uint64_t unit_id, const ObAddr &dst, const bool is_manual)
{
  int ret = OB_SUCCESS;
  lib::Worker::CompatMode compat_mode = lib::Worker::CompatMode::INVALID;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == unit_id || !dst.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(unit_id), K(dst), K(ret));
  } else if (OB_UNLIKELY(nullptr == srv_rpc_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("srv_rpc_proxy_ ptr is null", K(ret));
  } else {
    ObUnit *unit = NULL;
    share::ObResourcePool *pool = NULL;
    if (OB_FAIL(get_unit_by_id(unit_id, unit))) {
      LOG_WARN("get_unit_by_id failed", K(unit_id), K(ret));
    } else if (NULL == unit) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit is null", KP(unit), K(ret));
    } else if (ObUnit::UNIT_STATUS_ACTIVE != unit->status_) {
      ret = OB_OP_NOT_ALLOW;
      LOG_WARN("cannot migrate unit which is in deleting", K(ret), K(unit_id));
    } else if (OB_FAIL(get_resource_pool_by_id(unit->resource_pool_id_, pool))) {
      LOG_WARN("get_resource_pool_by_id failed",
          "resource pool id", unit->resource_pool_id_, K(ret));
    } else if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pool is null", KP(pool), K(ret));
    } else if (nullptr == schema_service_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("schema service ptr is null", K(ret));
    } else if (!pool->is_granted_to_tenant()) {
        // by pass
    } else if (OB_FAIL(ObCompatModeGetter::get_tenant_mode(pool->tenant_id_, compat_mode))) {
      LOG_WARN("fail to get tenant compat mode", K(ret));
    }
    if (OB_SUCC(ret)) {
      ObAddr src;
      ObZone zone;
      // granted: If the unit has not been assigned to the tenant, the migration can be performed immediately
      const bool granted = pool->is_granted_to_tenant();
      if (!granted
          && common::STANDBY_CLUSTER == ObClusterInfoGetter::get_cluster_role_v2()) {
        //Units without grant on the standby database are not allowed to be migrated
        ret = OB_OP_NOT_ALLOW;
        LOG_WARN("migrate not grant unit not valid", K(ret));
        LOG_USER_ERROR(OB_OP_NOT_ALLOW, "migrate unit which has not been granted");
      } else if (OB_FAIL(server_mgr_.get_server_zone(dst, zone))) {
        LOG_WARN("server_mgr_ get_server_zone failed", K(dst), K(ret));
      } else if (unit->server_ == dst) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unit->server same as migrate destination server",
            "unit", *unit, K(dst), K(ret));
      } else {
        src = unit->server_;
        if (unit->migrate_from_server_.is_valid()) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("unit is already migrating, cannot migrate any more", "unit", *unit, K(ret));
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "migrate unit already in migrating status");
        }

        if (OB_SUCC(ret)) {
          common::ObMySQLTransaction trans;
          ObUnit new_unit = *unit;
          new_unit.zone_ = zone;
          if (granted) {
            new_unit.migrate_from_server_ = unit->server_;
          }
          new_unit.server_ = dst;
          new_unit.is_manual_migrate_ = is_manual;
          const bool is_delete = false; // is_delete is false when migrate unit
          int tmp_ret = OB_SUCCESS;
          ObNotifyTenantServerResourceProxy notify_proxy(
                                            *srv_rpc_proxy_,
                                            &obrpc::ObSrvRpcProxy::notify_tenant_server_unit_resource);
          if (OB_FAIL(try_notify_tenant_server_unit_resource(
                  pool->tenant_id_, is_delete, notify_proxy,
                  *pool, compat_mode, new_unit, false/*if not grant*/,
                  false/*skip offline server*/))) {
            LOG_WARN("fail to try notify server unit resource", K(ret));
          }

          if (OB_SUCCESS != (tmp_ret = notify_proxy.wait())) {
            LOG_WARN("fail to wait notify resource", K(ret), K(tmp_ret));
            ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
          }

          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
            LOG_WARN("failed to start trans", K(ret));
          } else if (is_manual) {
            char ip_buf[common::MAX_IP_ADDR_LENGTH];
            (void)dst.ip_to_string(ip_buf, common::MAX_IP_ADDR_LENGTH);
            const int64_t job_id = RS_JOB_CREATE(MIGRATE_UNIT, trans, "unit_id", unit_id,
                "svr_ip", ip_buf, "svr_port", dst.get_port(), "tenant_id", pool->tenant_id_);
            if (job_id < 1) {
              ret = OB_SQL_OPT_ERROR;
              LOG_WARN("insert into all_rootservice_job failed ", K(ret));
            } else if (!granted) {
              if (OB_FAIL(RS_JOB_COMPLETE(job_id, OB_SUCCESS, trans))) {
                LOG_WARN("all_rootservice_job update failed", K(ret), K(job_id));
              }
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
            LOG_WARN("update_unit failed", K(new_unit), K(ret));
          } else {
            *unit = new_unit;
          }
          if (trans.is_started()) {
            if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
              ret = OB_SUCC(ret) ? tmp_ret : ret;
              LOG_WARN("trans commit failed", K(tmp_ret), K(ret));
            }
          }
        }

        // delete unit load if needed, insert unit load on dst
        ObUnitLoad load;
        if (OB_SUCC(ret)) {
          root_balance_->wakeup();
          if (!granted) {
            if (OB_FAIL(delete_unit_load(src, unit_id))) {
              LOG_WARN("delete_unit_load failed", K(src), K(unit_id), K(ret));
            }
          }

          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(gen_unit_load(unit, load))) {
            LOG_WARN("gen_unit_load failed", "unit", *unit, K(ret));
          } else if (OB_FAIL(insert_unit_load(dst, load))) {
            LOG_WARN("insert_unit_load failed", K(dst), K(ret));
          }
        }

        if (OB_SUCC(ret)) {
          if (granted) {
            //ObArray<ObAddr> servers;
            if (OB_FAIL(insert_migrate_unit(unit->migrate_from_server_, unit->unit_id_))) {
              LOG_WARN("insert_migrate_unit failed", "unit", *unit, K(ret));
            }
          }

          if (OB_SUCC(ret)) {
            ROOTSERVICE_EVENT_ADD("unit", "migrate_unit",
                "unit_id", unit->unit_id_,
                "migrate_from_server", unit->migrate_from_server_,
                "server", unit->server_,
                "tenant_id", pool->tenant_id_);
          }
        }
      }
      LOG_INFO("migrate unit succeed", K(unit_id), K(src), K(dst), K(granted));
    }
  }
  LOG_INFO("migrate unit", K(unit_id), K(dst), K(ret));
  return ret;
}

int ObUnitManager::inner_try_delete_migrate_unit_resource(
    const uint64_t unit_id,
    const common::ObAddr &migrate_from_server)
{
  int ret = OB_SUCCESS;
  ObUnit *unit = NULL;
  share::ObResourcePool *pool = NULL;
  share::ObUnitConfig *unit_config = nullptr;
  lib::Worker::CompatMode compat_mode = lib::Worker::CompatMode::INVALID;
  bool is_alive = false;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner stat unexpected", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(OB_INVALID_ID == unit_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(unit_id));
  } else if (OB_FAIL(get_unit_by_id(unit_id, unit))) {
    LOG_WARN("fail to get unit by id", K(ret), K(unit_id));
  } else if (OB_ISNULL(unit)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unit ptr is null", K(ret), KP(unit));
  } else if (!migrate_from_server.is_valid()) {
    LOG_INFO("unit not in migrating, no need to delete src resource", K(unit_id));
  } else if (OB_FAIL(server_mgr_.check_server_alive(migrate_from_server, is_alive))) {
    LOG_WARN("fail to check server alive", K(ret), "server", migrate_from_server);
  } else if (!is_alive) {
    LOG_INFO("src server not alive, ignore notify",
             K(unit_id), "server", migrate_from_server);
  } else if (OB_FAIL(get_resource_pool_by_id(unit->resource_pool_id_, pool))) {
    LOG_WARN("failed to get pool", K(ret), K(unit));
  } else if (OB_ISNULL(pool)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool ptr is null", K(ret), KP(pool));
  } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, unit_config))) {
    LOG_WARN("fail to get unit config by id", K(ret));
  } else if (OB_UNLIKELY(nullptr == unit_config)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unit config is null", K(ret), "unit_config_id", pool->unit_config_id_);
  } else if (!pool->is_granted_to_tenant()) {
    LOG_INFO("unit is not granted to any tenant", K(ret), "tenant_id", pool->tenant_id_);
  } else if (OB_FAIL(ObCompatModeGetter::get_tenant_mode(pool->tenant_id_, compat_mode))) {
    LOG_WARN("fail to get tenant compat mode", K(ret),
             "tenant_id", pool->tenant_id_, K(unit_id), "pool", *pool);
  } else {
    const int64_t rpc_timeout = NOTIFY_RESOURCE_RPC_TIMEOUT;
    obrpc::TenantServerUnitConfig tenant_unit_server_config;
    ObNotifyTenantServerResourceProxy notify_proxy(
                                      *srv_rpc_proxy_,
                                      &obrpc::ObSrvRpcProxy::notify_tenant_server_unit_resource);
    if (OB_FAIL(tenant_unit_server_config.init(
          pool->tenant_id_, unit->unit_id_, compat_mode, *unit_config,
          unit->replica_type_, false/*if not grant*/, true/*delete*/))) {
      LOG_WARN("fail to init tenant server unit config", K(ret), "tenant_id", pool->tenant_id_);
    } else if (OB_FAIL(notify_proxy.call(
            migrate_from_server, rpc_timeout, tenant_unit_server_config))) {
      LOG_WARN("fail to call notify resource to server",
               K(ret), K(rpc_timeout), "unit", *unit, "dest", migrate_from_server);
    }
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = notify_proxy.wait())) {
      LOG_WARN("fail to wait notify resource", K(ret), K(tmp_ret));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_SUCC(ret)) {
      LOG_INFO("notify resource to server succeed", "unit", *unit, "dest", migrate_from_server);
    }
  }
  return ret;
}

int ObUnitManager::end_migrate_unit(const uint64_t unit_id, const EndMigrateOp end_migrate_op)
{
  int ret = OB_SUCCESS;
  uint64_t tenant_id = OB_INVALID_ID;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(unit_id), K(ret));
  } else if (end_migrate_op < COMMIT || end_migrate_op > REVERSE) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid end_migrate_op", K(end_migrate_op), K(ret));
  } else {
    ObUnit *unit = NULL;
    common::ObMySQLTransaction trans;
    if (OB_FAIL(get_unit_by_id(unit_id, unit))) {
      LOG_WARN("get_unit_by_id failed", K(unit_id), K(ret));
    } else if (NULL == unit) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unit is null", KP(unit), K(ret));
    } else if (!unit->migrate_from_server_.is_valid()) {
      // FIXME(jingqian): when can this happened, figure it out
      ret = OB_SUCCESS;
      LOG_WARN("unit is not in migrating status, maybe end_migrate_unit has ever called",
               "unit", *unit, K(ret));
    } else {
      const ObAddr migrate_from_server = unit->migrate_from_server_;
      const ObAddr unit_server = unit->server_;
      ObUnit new_unit = *unit;
      new_unit.is_manual_migrate_ = false;  // clear manual_migrate
      // generate new unit
      if (COMMIT == end_migrate_op) {
        new_unit.migrate_from_server_.reset();
      } else if (ABORT == end_migrate_op) {
        new_unit.server_ = unit->migrate_from_server_;
        new_unit.migrate_from_server_.reset();
      } else {
        new_unit.server_ = unit->migrate_from_server_;
        new_unit.migrate_from_server_ = unit->server_;
      }

      // update unit in sys_table and in memory
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(trans.start(proxy_, OB_SYS_TENANT_ID))) {
        LOG_WARN("failed to start transaction ", K(ret));
      } else if (OB_FAIL(ut_operator_.update_unit(trans, new_unit))) {
        LOG_WARN("ut_operator update unit failed", K(new_unit), K(ret));
      } else {
        if (ABORT == end_migrate_op) {
          if (OB_FAIL(delete_unit_load(unit->server_, unit->unit_id_))) {
            LOG_WARN("delete_unit_load failed", "unit", *unit, K(ret));
          }
        } else if (COMMIT == end_migrate_op) {
          if (OB_FAIL(delete_unit_load(unit->migrate_from_server_, unit->unit_id_))) {
            LOG_WARN("delete_unit_load failed", "unit", *unit, K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          *unit = new_unit;
        }
      }

      // delete migrating unit from migrate_units of migrate_from_server,
      // if REVERSE == op, add migrating unit to migrate_units of unit_server
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(delete_migrate_unit(migrate_from_server, unit->unit_id_))) {
        LOG_WARN("delete_migrate_unit failed", K(migrate_from_server),
                 "unit_id", unit->unit_id_, K(ret));
      } else if (REVERSE == end_migrate_op) {
        if (OB_FAIL(insert_migrate_unit(unit_server, unit->unit_id_))) {
          LOG_WARN("insert_migrate_unit failed", K(unit_server), "unit_id",
                   unit->unit_id_, K(ret));
        }
      }

      if (OB_SUCC(ret)) {
        int tmp_ret = OB_SUCCESS;
        share::ObResourcePool *pool = NULL;
        if (OB_SUCCESS != (tmp_ret = get_resource_pool_by_id(unit->resource_pool_id_, pool))) {
          LOG_WARN("failed to get pool", K(tmp_ret), K(unit));
        } else {
          tenant_id = pool->tenant_id_;
        }
        ROOTSERVICE_EVENT_ADD("unit", "finish_migrate_unit",
                              "unit_id", unit_id,
                              "end_op", end_migrate_op,
                              "migrate_from_server", migrate_from_server,
                              "server", unit_server,
                              "tenant_id", tenant_id);

        // complete the job if exists
        char ip_buf[common::MAX_IP_ADDR_LENGTH];
        (void)unit_server.ip_to_string(ip_buf, common::MAX_IP_ADDR_LENGTH);
        ObRsJobInfo job_info;
        tmp_ret = RS_JOB_FIND(job_info, trans, "job_type", "MIGRATE_UNIT",
                              "job_status", "INPROGRESS", "unit_id", unit_id,
                              "svr_ip", ip_buf, "svr_port", unit_server.get_port());
        if (OB_SUCCESS == tmp_ret && job_info.job_id_ > 0) {
          tmp_ret = (end_migrate_op == COMMIT) ? OB_SUCCESS :
              (end_migrate_op == REVERSE ? OB_CANCELED : OB_TIMEOUT);
          if (OB_FAIL(RS_JOB_COMPLETE(job_info.job_id_, tmp_ret, trans))) {
            LOG_WARN("all_rootservice_job update failed", K(ret), K(job_info));
          }
        } else {
          //Can not find the situation, only the user manually opened will write rs_job
          LOG_WARN("no rs job", K(ret), K(tmp_ret), K(unit_id));
        }
      }
      const bool commit = OB_SUCC(ret) ? true:false;
      int tmp_ret = OB_SUCCESS ;
      if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
        LOG_WARN("tran commit failed", K(tmp_ret));
      }
      ret = OB_SUCC(ret) ? tmp_ret : ret;

      if (OB_SUCC(ret) && COMMIT == end_migrate_op && OB_INVALID_ID != tenant_id) {
        (void)inner_try_delete_migrate_unit_resource(unit_id, migrate_from_server);
      }
    }
  }

  LOG_INFO("end migrate unit", K(unit_id), K(end_migrate_op), K(ret));
  return ret;
}

#define INSERT_ITEM_TO_MAP(map, key, pvalue) \
  do { \
    if (OB_FAIL(ret)) { \
    } else if (OB_FAIL(map.set_refactored(key, pvalue))) { \
      if (OB_HASH_EXIST == ret) { \
        LOG_WARN("key already exist", K(key), K(ret)); \
      } else { \
        LOG_WARN("map set failed", K(ret)); \
      } \
    } else { \
    } \
  } while (false)

#define SET_ITEM_TO_MAP(map, key, value) \
  do { \
    const int overwrite = 1; \
    if (OB_FAIL(ret)) { \
    } else if (OB_FAIL(map.set_refactored(key, value, overwrite))) { \
      LOG_WARN("map set failed", K(ret)); \
    } else { \
    } \
  } while (false)

#define INSERT_ARRAY_TO_MAP(map, key, array) \
  do { \
    if (OB_FAIL(ret)) { \
    } else if (OB_FAIL(map.set_refactored(key, array))) { \
      if (OB_HASH_EXIST == ret) { \
        LOG_WARN("key already exist", K(key), K(ret)); \
      } else { \
        LOG_WARN("map set failed", K(ret)); \
      } \
    } else { \
    } \
  } while (false)

int ObUnitManager::build_unit_map(const ObIArray<ObUnit> &units)
{
  int ret = OB_SUCCESS;
  // units is empty if invoked during bootstrap
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(id_unit_map_.clear())) {
    LOG_WARN("id_unit_map_ clear failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units.count(); ++i) {
      ObUnit *unit = NULL;
      if (NULL == (unit = allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc unit failed", K(ret));
      } else {
        *unit = units.at(i);
        if (OB_FAIL(insert_unit(unit))) {
          LOG_WARN("insert_unit failed", "unit", *unit, K(ret));
        }

        if (OB_FAIL(ret)) {
          //avoid memory leak
          allocator_.free(unit);
          unit = NULL;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::build_config_map(const ObIArray<ObUnitConfig> &configs)
{
  int ret = OB_SUCCESS;
  // configs is empty if invoked during bootstrap
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(id_config_map_.clear())) {
    LOG_WARN("id_config_map_ clear failed", K(ret));
  } else if (OB_FAIL(name_config_map_.clear())) {
    LOG_WARN("name_config_map_ clear failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < configs.count(); ++i) {
      ObUnitConfig *config = NULL;
      if (NULL == (config = config_allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc unit config failed", K(ret));
      } else {
        *config = configs.at(i);
        if (OB_FAIL(insert_unit_config(config))) {
          LOG_WARN("insert_unit_config failed", KP(config), K(ret));
        }

        if (OB_FAIL(ret)) {
          config_allocator_.free(config);
          config = NULL;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::build_pool_map(const ObIArray<share::ObResourcePool> &pools)
{
  int ret = OB_SUCCESS;
  // pools is empty if invoked during bootstrap
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(id_pool_map_.clear())) {
    LOG_WARN("id_pool_map_ clear failed", K(ret));
  } else if (OB_FAIL(name_pool_map_.clear())) {
    LOG_WARN("name_pool_map_ clear failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool = NULL;
      if (NULL == (pool = pool_allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc resource pool failed", K(ret));
      } else {
        if (OB_FAIL(pool->assign(pools.at(i)))) {
          LOG_WARN("failed to assign pool", K(ret));
        } else {
          INSERT_ITEM_TO_MAP(id_pool_map_, pool->resource_pool_id_, pool);
          INSERT_ITEM_TO_MAP(name_pool_map_, pool->name_, pool);
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(inc_config_ref_count(pool->unit_config_id_))) {
            LOG_WARN("inc_config_ref_count failed", "config id", pool->unit_config_id_, K(ret));
          } else if (OB_FAIL(insert_config_pool(pool->unit_config_id_, pool))) {
            LOG_WARN("insert config pool failed", "config id", pool->unit_config_id_, K(ret));
          }
        }

        if (OB_FAIL(ret)) {
          pool_allocator_.free(pool);
          pool = NULL;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::insert_unit_config(ObUnitConfig *config)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (NULL == config) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("config is null", KP(config), K(ret));
  } else if (!config->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid config", "config", *config, K(ret));
  } else {
    INSERT_ITEM_TO_MAP(id_config_map_, config->unit_config_id(), config);
    INSERT_ITEM_TO_MAP(name_config_map_, config->name(), config);
    int64_t ref_count = 0;
    SET_ITEM_TO_MAP(config_ref_count_map_, config->unit_config_id(), ref_count);
  }
  return ret;
}

int ObUnitManager::inc_config_ref_count(const uint64_t config_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid config_id", K(config_id), K(ret));
  } else {
    int64_t ref_count = 0;
    if (OB_FAIL(get_config_ref_count(config_id, ref_count))) {
      LOG_WARN("get_config_ref_count failed", K(config_id), K(ret));
    } else {
      ++ref_count;
      SET_ITEM_TO_MAP(config_ref_count_map_, config_id, ref_count);
    }
  }
  return ret;
}

int ObUnitManager::dec_config_ref_count(const uint64_t config_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid config_id", K(config_id), K(ret));
  } else {
    int64_t ref_count = 0;
    if (OB_FAIL(get_config_ref_count(config_id, ref_count))) {
      LOG_WARN("get_config_ref_count failed", K(config_id), K(ret));
    } else {
      --ref_count;
      SET_ITEM_TO_MAP(config_ref_count_map_, config_id, ref_count);
    }
  }
  return ret;
}

int ObUnitManager::update_pool_map(share::ObResourcePool *resource_pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (NULL == resource_pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("resource_pool is null", KP(resource_pool), K(ret));
  } else if (!resource_pool->is_valid() || OB_INVALID_ID == resource_pool->resource_pool_id_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid resource_pool", "resource_pool", *resource_pool, K(ret));
  } else {
    INSERT_ITEM_TO_MAP(id_pool_map_, resource_pool->resource_pool_id_, resource_pool);
    INSERT_ITEM_TO_MAP(name_pool_map_, resource_pool->name_, resource_pool);
  }
  return ret;
}

int ObUnitManager::insert_unit(ObUnit *unit)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_INFO("not init", K(ret));
  } else if (NULL == unit) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit is null", KP(unit), K(ret));
  } else if (!unit->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit", "unit", *unit, K(ret));
  } else {
    ObArray<ObUnit *> *units = NULL;
    if (OB_FAIL(get_units_by_pool(unit->resource_pool_id_, units))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get_units_by_pool failed", K(ret));
      } else {
        ret = OB_SUCCESS;
        if (NULL == (units = pool_unit_allocator_.alloc())) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_ERROR("alloc ObArray<ObUnit *> failed", K(ret));
        } else {
          if (OB_FAIL(units->push_back(unit))) {
            LOG_WARN("push_back failed", K(ret));
          } else {
            INSERT_ARRAY_TO_MAP(pool_unit_map_, unit->resource_pool_id_, units);
          }
          if (OB_FAIL(ret)) {
            pool_unit_allocator_.free(units);
            units = NULL;
          }
        }
      }
    } else if (NULL == units) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("units is null", KP(units), K(ret));
    } else if (OB_FAIL(units->push_back(unit))) {
      LOG_WARN("push_back failed", K(ret));
    }

    if (OB_SUCC(ret)) {
      INSERT_ITEM_TO_MAP(id_unit_map_, unit->unit_id_, unit);

      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(insert_unit_loads(unit))) {
        LOG_WARN("insert_unit_loads failed", "unit", *unit, K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::insert_unit_loads(ObUnit *unit)
{
  int ret = OB_SUCCESS;
  ObUnitLoad load;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (NULL == unit) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit is null", KP(unit), K(ret));
  } else if (!unit->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit", "unit", *unit, K(ret));
  } else if (OB_FAIL(gen_unit_load(unit, load))) {
    LOG_WARN("gen_unit_load failed", "unit", *unit, K(ret));
  } else if (OB_FAIL(insert_unit_load(unit->server_, load))) {
    LOG_WARN("insert_unit_load failed", "server", unit->server_, K(ret));
  } else {
    if (OB_SUCCESS == ret && unit->migrate_from_server_.is_valid()) {
      if (OB_FAIL(insert_unit_load(unit->migrate_from_server_, load))) {
        LOG_WARN("insert_unit_load failed", "server", unit->migrate_from_server_, K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::insert_unit_load(const ObAddr &server, const ObUnitLoad &load)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnitLoad> *loads = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!server.is_valid() || !load.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(server), K(load), K(ret));
  } else if (OB_FAIL(get_loads_by_server(server, loads))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_loads_by_server failed", K(server), K(ret));
    } else {
      ret = OB_SUCCESS;
      // not exist, alloc new array, add to hash map
      if (NULL == (loads = load_allocator_.alloc())) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_ERROR("alloc ObArray<ObUnitLoad> failed", K(ret));
      } else {
        if (OB_FAIL(loads->push_back(load))) {
          LOG_WARN("push_back failed", K(ret));
        } else if (OB_FAIL(insert_load_array(server, loads))) {
          LOG_WARN("insert_unit_load failed", K(server), K(ret));
        }
        if (OB_FAIL(ret)) {
          // avoid memory leak
          load_allocator_.free(loads);
          loads = NULL;
        }
      }
    }
  } else if (NULL == loads) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("loads is null", KP(loads), K(ret));
  } else {
    if (OB_FAIL(loads->push_back(load))) {
      LOG_WARN("push_back failed", K(ret));
    }
  }
  return ret;
}

int ObUnitManager::insert_load_array(const ObAddr &addr, ObArray<ObUnitLoad> *loads)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!addr.is_valid() || NULL == loads) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(addr), KP(loads), K(ret));
  } else if (loads->count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("loads is empty", "loads count", loads->count(), K(ret));
  } else {
    if (OB_FAIL(server_loads_.set_refactored(addr, loads))) {
      if (OB_HASH_EXIST == ret) {
        LOG_WARN("load array is not expect to exist", K(addr), K(ret));
      } else {
        LOG_WARN("set failed", K(addr), K(ret));
      }
    } else {
      // do nothing
    }
  }
  return ret;
}

int ObUnitManager::update_pool_load(share::ObResourcePool *pool,
    share::ObUnitConfig *new_config)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (NULL == pool || NULL == new_config) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(pool), KP(new_config));
  } else {
    FOREACH_X(sl, server_loads_, OB_SUCC(ret)) {
      if (NULL == sl->second) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL value", K(ret));
      }
      FOREACH_X(l, *sl->second, OB_SUCC(ret)) {
        if (l->pool_ == pool) {
          l->unit_config_ = new_config;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::gen_unit_load(ObUnit *unit, ObUnitLoad &load) const
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (NULL == unit) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit is null", KP(unit), K(ret));
  } else if (!unit->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("unit is invalid", "unit", *unit, K(ret));
  } else {
    ObUnitConfig *config = NULL;
    share::ObResourcePool *pool = NULL;
    if (OB_FAIL(get_resource_pool_by_id(unit->resource_pool_id_, pool))) {
      LOG_WARN("get_resource_pool_by_id failed", "pool id", unit->resource_pool_id_, K(ret));
    } else if (NULL == pool) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", KP(pool), K(ret));
    } else if (OB_FAIL(get_unit_config_by_id(pool->unit_config_id_, config))) {
      LOG_WARN("get_unit_config_by_id failed", "unit config id", pool->unit_config_id_, K(ret));
    } else if (NULL == config) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("config is null", KP(config), K(ret));
    } else {
      load.unit_ = unit;
      load.pool_ = pool;
      load.unit_config_ = config;
    }
  }
  return ret;
}

int ObUnitManager::gen_unit_load(const uint64_t unit_id, ObUnitLoad &load) const
{
  int ret = OB_SUCCESS;
  ObUnit *unit = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit_id", K(unit_id), K(ret));
  } else if (OB_FAIL(get_unit_by_id(unit_id, unit))) {
    LOG_WARN("get_unit_by_id failed", K(unit_id), K(ret));
  } else if (NULL == unit) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unit is null", KP(unit), K(ret));
  } else if (OB_FAIL(gen_unit_load(unit, load))) {
    LOG_WARN("gen_unit_load failed", "unit", *unit, K(ret));
  }
  return ret;
}

int ObUnitManager::insert_tenant_pool(const uint64_t tenant_id, share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!is_valid_tenant_id(tenant_id) || NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), KP(pool), K(ret));
  } else if (OB_FAIL(insert_id_pool(tenant_pools_map_,
      tenant_pools_allocator_, tenant_id, pool))) {
    LOG_WARN("insert tenant pool failed", K(tenant_id), KP(pool), K(ret));
  }

  return ret;
}

int ObUnitManager::insert_config_pool(const uint64_t config_id, share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id || NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(config_id), KP(pool), K(ret));
  } else if (OB_FAIL(insert_id_pool(config_pools_map_,
      config_pools_allocator_, config_id, pool))) {
    LOG_WARN("insert config pool failed", K(config_id), KP(pool), K(ret));
  }

  return ret;
}

int ObUnitManager::insert_id_pool(
    common::hash::ObHashMap<uint64_t, common::ObArray<share::ObResourcePool *> *> &map,
    common::ObPooledAllocator<common::ObArray<share::ObResourcePool *> > &allocator,
    const uint64_t id,
    share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!map.created() || OB_INVALID_ID == id || NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", "map created", map.created(), K(id), KP(pool), K(ret));
  } else {
    ObArray<share::ObResourcePool *> *pools = NULL;
    if (OB_FAIL(get_pools_by_id(map, id, pools))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get_pools_by_id failed", K(id), K(ret));
      } else {
        ret = OB_SUCCESS;
        if (NULL == (pools = allocator.alloc())) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_ERROR("alloc pools failed", K(ret));
        } else if (OB_FAIL(pools->push_back(pool))) {
          LOG_WARN("push_back failed", K(ret));
        } else if (OB_FAIL(insert_id_pool_array(map, id, pools))) {
          LOG_WARN("insert_id_pool_array failed", K(id), K(ret));
        }

        // avoid memory leak
        if (OB_SUCCESS != ret && NULL != pools) {
          allocator.free(pools);
          pools = NULL;
        }
      }
    } else if (NULL == pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", KP(pools), K(ret));
    } else if (!has_exist_in_array(*pools, pool)) {
      if (OB_FAIL(pools->push_back(pool))) {
        LOG_WARN("push_back failed", K(ret));
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("resource pool already exists", K(ret), K(id), K(pools), K(pool));
    }
  }
  return ret;
}

int ObUnitManager::insert_id_pool_array(
    common::hash::ObHashMap<uint64_t, common::ObArray<share::ObResourcePool *> *> &map,
    const uint64_t id,
    ObArray<share::ObResourcePool *> *pools)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!map.created() || OB_INVALID_ID == id || NULL == pools) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", "map created", map.created(), K(id), KP(pools), K(ret));
  } else {
    if (OB_FAIL(map.set_refactored(id, pools))) {
      if (OB_HASH_EXIST == ret) {
        LOG_WARN("pools is not expect to exist", K(id), K(ret));
      } else {
        LOG_WARN("set failed", K(id), K(ret));
      }
    } else {
      // do nothing
    }
  }
  return ret;
}

int ObUnitManager::insert_migrate_unit(const ObAddr &src_server, const uint64_t unit_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!src_server.is_valid() || OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(src_server), K(unit_id), K(ret));
  } else {
    ObArray<uint64_t> *migrate_units = NULL;
    if (OB_FAIL(get_migrate_units_by_server(src_server, migrate_units))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get_migrate_units_by_server failed", K(src_server), K(ret));
      } else {
        ret= OB_SUCCESS;
        if (NULL == (migrate_units = migrate_units_allocator_.alloc())) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_ERROR("alloc  failed", K(ret));
        } else if (OB_FAIL(migrate_units->push_back(unit_id))) {
          LOG_WARN("push_back failed", K(ret));
        } else {
          INSERT_ARRAY_TO_MAP(server_migrate_units_map_, src_server, migrate_units);
        }

        // avoid memory leak
        if (OB_SUCCESS != ret && NULL != migrate_units) {
          migrate_units_allocator_.free(migrate_units);
          migrate_units = NULL;
        }
      }
    } else if (NULL == migrate_units) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("migrate_units is null", KP(migrate_units), K(ret));
    } else {
      if (OB_FAIL(migrate_units->push_back(unit_id))) {
        LOG_WARN("push_back failed", K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::set_server_ref_count(common::hash::ObHashMap<ObAddr, int64_t> &map,
    const ObAddr &server, const int64_t ref_count) const
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!map.created() || !server.is_valid() || ref_count <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", "map_created", map.created(), K(server), K(ref_count), K(ret));
  } else {
    SET_ITEM_TO_MAP(map, server, ref_count);
  }
  return ret;
}


#undef INSERT_ITEM_TO_MAP
#undef SET_ITEM_TO_MAP
#undef INSERT_ARRAY_TO_MAP

#define DELETE_ITEM_FROM_MAP(map, key) \
  do { \
    if (OB_FAIL(ret)) { \
    } else if (OB_FAIL(map.erase_refactored(key))) { \
      LOG_WARN("map erase failed", K(key), K(ret)); \
    } else { \
    } \
  } while (false)

int ObUnitManager::delete_unit_config(const uint64_t config_id,
                                      const ObUnitConfigName &name)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id || name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(config_id), K(name), K(ret));
  } else {
    DELETE_ITEM_FROM_MAP(id_config_map_, config_id);
    DELETE_ITEM_FROM_MAP(name_config_map_, name);
    DELETE_ITEM_FROM_MAP(config_ref_count_map_, config_id);
  }
  return ret;
}

int ObUnitManager::delete_resource_pool(const uint64_t pool_id,
                                        const ObResourcePoolName &name)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == pool_id || name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_id), K(name), K(ret));
  } else {
    DELETE_ITEM_FROM_MAP(id_pool_map_, pool_id);
    DELETE_ITEM_FROM_MAP(name_pool_map_, name);
  }
  return ret;
}

int ObUnitManager::delete_units_in_zones(
    const uint64_t resource_pool_id,
    const common::ObIArray<common::ObZone> &to_be_removed_zones)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnit *> *units = NULL;
  ObArray<ObUnit *> left_units;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(OB_INVALID_ID == resource_pool_id)
             || OB_UNLIKELY(to_be_removed_zones.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(resource_pool_id), K(to_be_removed_zones));
  } else if (get_units_by_pool(resource_pool_id, units)) {
    LOG_WARN("fail to get units by pool", K(ret), K(resource_pool_id));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units is null", K(ret), KP(units));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      const ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit should not be null", "unit", OB_P(units->at(i)), K(ret));
      } else if (!has_exist_in_array(to_be_removed_zones, unit->zone_)) {
        if (OB_FAIL(left_units.push_back(units->at(i)))) {
          LOG_WARN("fail to push back", K(ret));
        } else {} // no more to do
      } else if (OB_FAIL(delete_unit_loads(*unit))) {
        LOG_WARN("fail to delete unit load", K(ret), "unit", *units->at(i));
      } else {
        DELETE_ITEM_FROM_MAP(id_unit_map_, unit->unit_id_);
        if (OB_SUCC(ret)) {
          if (unit->migrate_from_server_.is_valid()) {
            //If the unit is being migrated, delete it from the state in memory
            if (OB_FAIL(delete_migrate_unit(unit->migrate_from_server_,
                                            unit->unit_id_))) {
              LOG_WARN("failed to delete migrate unit", K(ret), "unit", *unit);
            }
          }
        }
        if (OB_SUCC(ret)) {
          ROOTSERVICE_EVENT_ADD("unit", "drop_unit",
              "unit_id", units->at(i)->unit_id_);
          allocator_.free(units->at(i));
          units->at(i) = NULL;;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (left_units.count() <= 0) {
        DELETE_ITEM_FROM_MAP(pool_unit_map_, resource_pool_id);
        if (OB_SUCC(ret)) {
          pool_unit_allocator_.free(units);
          units = NULL;
        }
      } else {
        if (OB_FAIL(units->assign(left_units))) {
          LOG_WARN("assign failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_units_of_pool(const uint64_t resource_pool_id)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnit *> *units = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(resource_pool_id), K(ret));
  } else if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
    LOG_WARN("fail to get unit by pool", K(resource_pool_id), K(ret));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units is null", KP(units), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      const ObUnit *unit = units->at(i);
      if (OB_UNLIKELY(NULL == unit)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit should not be null", K(ret));
      } else if (OB_FAIL(delete_unit_loads(*unit))) {
        LOG_WARN("fail to delete unit load", K(ret));
      } else {
        DELETE_ITEM_FROM_MAP(id_unit_map_, unit->unit_id_);
        if (OB_SUCC(ret)) {
          if (unit->migrate_from_server_.is_valid()) {
            if (OB_FAIL(delete_migrate_unit(unit->migrate_from_server_,
                                            unit->unit_id_))) {
              LOG_WARN("failed to delete migrate unit", K(ret), "unit", *unit);
            }
          }

        }

        if (OB_SUCC(ret)) {
          ROOTSERVICE_EVENT_ADD("unit", "drop_unit",
                                "unit_id", units->at(i)->unit_id_);
          allocator_.free(units->at(i));
          units->at(i) = NULL;
        }
      }
    }
    if (OB_SUCC(ret)) {
      DELETE_ITEM_FROM_MAP(pool_unit_map_, resource_pool_id);
      if (OB_SUCC(ret)) {
        pool_unit_allocator_.free(units);
        units = NULL;
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_invalid_inmemory_units(
    const uint64_t resource_pool_id,
    const common::ObIArray<uint64_t> &valid_unit_ids)
{
  int ret = OB_SUCCESS;
  ObArray<share::ObUnit *> *units = NULL;
  ObArray<share::ObUnit *> left_units;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == resource_pool_id || valid_unit_ids.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(resource_pool_id),
             "valid_unit_cnt", valid_unit_ids.count());
  } else if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
    LOG_WARN("fail to get units by pool", K(ret), K(resource_pool_id));
  } else if (OB_UNLIKELY(NULL == units)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("units is null", KP(units), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      if (NULL == units->at(i)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unit ptr is null", K(ret));
      } else if (has_exist_in_array(valid_unit_ids, units->at(i)->unit_id_)) {
        if (OB_FAIL(left_units.push_back(units->at(i)))) {
          LOG_WARN("push_back failed", K(ret));
        }
      } else if (OB_FAIL(delete_unit_loads(*units->at(i)))) {
        LOG_WARN("fail to delete unit loads", K(ret), "unit", *units->at(i));
      } else {
        DELETE_ITEM_FROM_MAP(id_unit_map_, units->at(i)->unit_id_);
        if (OB_SUCC(ret)) {
          ROOTSERVICE_EVENT_ADD("unit", "drop_unit",
              "unit_id", units->at(i)->unit_id_);
          allocator_.free(units->at(i));
          units->at(i) = NULL;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (left_units.count() <= 0) {
        DELETE_ITEM_FROM_MAP(pool_unit_map_, resource_pool_id);
        if (OB_SUCC(ret)) {
          pool_unit_allocator_.free(units);
          units = NULL;
        }
      } else {
        if (OB_FAIL(units->assign(left_units))) {
          LOG_WARN("assign failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_inmemory_units(
    const uint64_t resource_pool_id,
    const common::ObIArray<uint64_t> &unit_ids)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == resource_pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(resource_pool_id), K(ret));
  } else {
    ObArray<ObUnit *> *units = NULL;
    ObArray<ObUnit *> left_units;
    if (OB_FAIL(get_units_by_pool(resource_pool_id, units))) {
      LOG_WARN("get_units_by_pool failed", K(resource_pool_id), K(ret));
    } else if (NULL == units) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("units is null", KP(units), K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
        if (NULL == units->at(i)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unit should not be null", "unit", OB_P(units->at(i)), K(ret));
        } else if (!has_exist_in_array(unit_ids, units->at(i)->unit_id_)) {
          if (OB_FAIL(left_units.push_back(units->at(i)))) {
            LOG_WARN("push_back failed", K(ret));
          }
        } else {
          if (OB_FAIL(delete_unit_loads(*units->at(i)))) {
            LOG_WARN("delete_unit_load failed", "unit", *units->at(i), K(ret));
          } else {
            DELETE_ITEM_FROM_MAP(id_unit_map_, units->at(i)->unit_id_);
            if (OB_SUCC(ret)) {
              ROOTSERVICE_EVENT_ADD("unit", "drop_unit",
                  "unit_id", units->at(i)->unit_id_);
              allocator_.free(units->at(i));
              units->at(i) = NULL;
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        // if all units of pool are deleted, delete item from hashmap
        if (left_units.count() <= 0) {
          DELETE_ITEM_FROM_MAP(pool_unit_map_, resource_pool_id);
          if (OB_SUCC(ret)) {
            pool_unit_allocator_.free(units);
            units = NULL;
          }
        } else {
          if (OB_FAIL(units->assign(left_units))) {
            LOG_WARN("assign failed", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_unit_loads(const ObUnit &unit)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!unit.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid unit", K(unit), K(ret));
  } else {
    const uint64_t unit_id = unit.unit_id_;
    if (OB_FAIL(delete_unit_load(unit.server_, unit_id))) {
      LOG_WARN("delete_unit_load failed", "server", unit.server_, K(unit_id), K(ret));
    } else {
      if (unit.migrate_from_server_.is_valid()) {
        if (OB_FAIL(delete_unit_load(unit.migrate_from_server_, unit_id))) {
          LOG_WARN("delete_unit_load failed", "server", unit.migrate_from_server_,
              K(unit_id), K(ret));
        } else {
          // do nothing
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_unit_load(const ObAddr &server, const uint64_t unit_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!server.is_valid() || OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(server), K(unit_id), K(ret));
  } else {
    ObArray<ObUnitLoad> *loads = NULL;
    if (OB_FAIL(get_loads_by_server(server, loads))) {
      LOG_WARN("get_loads_by_server failed", K(server), K(ret));
    } else if (NULL == loads) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("loads is null", KP(loads), K(ret));
    } else {
      int64_t index = -1;
      for (int64_t i = 0; OB_SUCC(ret) && i < loads->count(); ++i) {
        if (loads->at(i).unit_->unit_id_ == unit_id) {
          index = i;
          break;
        }
      }
      if (-1 == index) {
        ret = OB_ENTRY_NOT_EXIST;
        LOG_WARN("unit load not exist", K(server), K(unit_id), K(ret));
      } else {
        for (int64_t i = index; i < loads->count() - 1; ++i) {
          loads->at(i) = loads->at(i+1);
        }
        loads->pop_back();
        if (0 == loads->count()) {
          load_allocator_.free(loads);
          loads = NULL;
          DELETE_ITEM_FROM_MAP(server_loads_, server);
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_tenant_pool(const uint64_t tenant_id, share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!is_valid_tenant_id(tenant_id) || NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), KP(pool), K(ret));
  } else if (OB_FAIL(delete_id_pool(tenant_pools_map_,
      tenant_pools_allocator_, tenant_id, pool))) {
    LOG_WARN("delete tenant pool failed", K(ret), K(tenant_id), "pool", *pool);
  }
  return ret;
}

int ObUnitManager::delete_config_pool(const uint64_t config_id, share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id || NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(config_id), KP(pool), K(ret));
  } else if (OB_FAIL(delete_id_pool(config_pools_map_,
      config_pools_allocator_, config_id, pool))) {
    LOG_WARN("delete config pool failed", K(ret), K(config_id), "pool", *pool);
  }
  return ret;
}

int ObUnitManager::delete_id_pool(
    common::hash::ObHashMap<uint64_t, common::ObArray<share::ObResourcePool *> *> &map,
    common::ObPooledAllocator<common::ObArray<share::ObResourcePool *> > &allocator,
    const uint64_t id,
    share::ObResourcePool *pool)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!map.created() || OB_INVALID_ID == id || NULL == pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", "map created", map.created(), K(id), KP(pool), K(ret));
  } else {
    ObArray<share::ObResourcePool *> *pools = NULL;
    if (OB_FAIL(get_pools_by_id(map, id, pools))) {
      LOG_WARN("get_pools_by_id failed", K(id), K(ret));
    } else if (NULL == pools) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pools is null", KP(pools), K(ret));
    } else {
      int64_t index = -1;
      for (int64_t i = 0; i < pools->count(); ++i) {
        if (pools->at(i) == pool) {
          index = i;
          break;
        }
      }
      if (-1 == index) {
        ret = OB_ENTRY_NOT_EXIST;
        LOG_WARN("pool not exist", K(id), K(ret));
      } else if (OB_FAIL(pools->remove(index))) {
        LOG_WARN("remove failed", K(index), K(ret));
      } else if (0 == pools->count()) {
        allocator.free(pools);
        pools = NULL;
        DELETE_ITEM_FROM_MAP(map, id);
      }
    }
  }
  return ret;
}

int ObUnitManager::delete_migrate_unit(const ObAddr &src_server,
                                       const uint64_t unit_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!src_server.is_valid() || OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(src_server), K(unit_id), K(ret));
  } else {
    ObArray<uint64_t> *migrate_units = NULL;
    if (OB_FAIL(get_migrate_units_by_server(src_server, migrate_units))) {
      LOG_WARN("get_migrate_units_by_server failed", K(src_server), K(ret));
    } else if (NULL == migrate_units) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("migrate_units is null", KP(migrate_units), K(ret));
    } else {
      int64_t index = -1;
      for (int64_t i = 0; i < migrate_units->count(); ++i) {
        if (migrate_units->at(i) == unit_id) {
          index = i;
          break;
        }
      }
      if (-1 == index) {
        ret = OB_ENTRY_NOT_EXIST;
        LOG_WARN("migrate_unit not exist", K(unit_id), K(ret));
      } else if (OB_FAIL(migrate_units->remove(index))) {
        LOG_WARN("remove failed", K(index), K(ret));
      } else if (0 == migrate_units->count()) {
        migrate_units_allocator_.free(migrate_units);
        migrate_units = NULL;
        DELETE_ITEM_FROM_MAP(server_migrate_units_map_, src_server);
      }
    }
  }
  return ret;
}

#undef DELETE_ITEM_FROM_MAP

#define GET_ITEM_FROM_MAP(map, key, value) \
  do { \
    if (OB_FAIL(map.get_refactored(key, value))) { \
      if (OB_HASH_NOT_EXIST == ret) { \
        ret = OB_ENTRY_NOT_EXIST; \
      } else { \
        LOG_WARN("map get failed", K(key), K(ret)); \
      } \
    } else { \
    } \
  } while (false)

int ObUnitManager::update_unit_load(
    share::ObUnit *unit,
    share::ObResourcePool *new_pool)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (NULL == unit || NULL == new_pool) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(unit), KP(new_pool));
  } else {
    ObArray<ObUnitManager::ObUnitLoad> *server_load = NULL;
    GET_ITEM_FROM_MAP(server_loads_, unit->server_, server_load);
    if (OB_FAIL(ret)) {
      LOG_WARN("fail to get server load", K(ret), "server", unit->server_);
    } else if (NULL == server_load) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("loads ptr is null", K(ret));
    } else {
      FOREACH_X(l, *server_load, OB_SUCC(ret)) {
        if (l->unit_ == unit) {
          l->pool_ = new_pool;
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (unit->migrate_from_server_.is_valid()) {
      ObArray<ObUnitManager::ObUnitLoad> *migrate_from_load = NULL;
      GET_ITEM_FROM_MAP(server_loads_, unit->migrate_from_server_, migrate_from_load);
      if (OB_FAIL(ret)) {
         LOG_WARN("fail to get server load", K(ret),
                  "migrate_from_server", unit->migrate_from_server_);
      } else if (NULL == migrate_from_load) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("loads ptr is null", K(ret));
      } else {
        FOREACH_X(l, *migrate_from_load, OB_SUCC(ret)) {
          if (l->unit_ == unit) {
            l->pool_ = new_pool;
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_unit_config_by_name(const ObUnitConfigName &name,
                                           ObUnitConfig *&config) const
{
  int ret = OB_SUCCESS;
  config = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid name", K(name), K(ret));
  } else {
    GET_ITEM_FROM_MAP(name_config_map_, name, config);
  }
  return ret;
}

int ObUnitManager::get_unit_config_by_id(const uint64_t config_id, ObUnitConfig *&config) const
{
  int ret = OB_SUCCESS;
  config = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(config_id), K(ret));
  } else {
    GET_ITEM_FROM_MAP(id_config_map_, config_id, config);
  }
  return ret;
}

int ObUnitManager::get_config_ref_count(const uint64_t config_id, int64_t &ref_count) const
{
  int ret = OB_SUCCESS;
  ref_count = 0;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(config_id), K(ret));
  } else {
    GET_ITEM_FROM_MAP(config_ref_count_map_, config_id, ref_count);
    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
        LOG_WARN("GET_ITEM_FROM_MAP failed", K(config_id), K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::get_server_ref_count(common::hash::ObHashMap<ObAddr, int64_t> &map,
    const ObAddr &server, int64_t &ref_count) const
{
  int ret = OB_SUCCESS;
  ref_count = 0;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!map.created() || !server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", "map created", map.created(), K(server), K(ret));
  } else {
    GET_ITEM_FROM_MAP(map, server, ref_count);
    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
        LOG_WARN("GET_ITEM_FROM_MAP failed", K(server), K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::get_resource_pool_by_name(
    const ObResourcePoolName &name,
    share::ObResourcePool &pool) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  share::ObResourcePool *inner_pool = nullptr;
  if (OB_FAIL(inner_get_resource_pool_by_name(name, inner_pool))) {
    LOG_WARN("fail to inner get resource pool by name", K(ret), K(name));
  } else if (OB_UNLIKELY(nullptr == inner_pool)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("inner pool ptr is null", K(ret));
  } else if (OB_FAIL(pool.assign(*inner_pool))) {
    LOG_WARN("fail to assign pool", K(ret));
  }
  return ret;
}

int ObUnitManager::inner_get_resource_pool_by_name(
    const ObResourcePoolName &name,
    share::ObResourcePool *&pool) const
{
  int ret = OB_SUCCESS;
  pool = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (name.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid name", K(name), K(ret));
  } else {
    GET_ITEM_FROM_MAP(name_pool_map_, name, pool);
  }
  return ret;
}

int ObUnitManager::get_resource_pool_by_id(const uint64_t pool_id, share::ObResourcePool *&pool) const
{
  int ret = OB_SUCCESS;
  pool = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_id), K(ret));
  } else {
    GET_ITEM_FROM_MAP(id_pool_map_, pool_id, pool);
  }
  return ret;
}

int ObUnitManager::get_units_by_pool(const uint64_t pool_id, ObArray<ObUnit *> *&units) const
{
  int ret = OB_SUCCESS;
  units = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == pool_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(pool_id), K(ret));
  } else {
    GET_ITEM_FROM_MAP(pool_unit_map_, pool_id, units);
  }
  return ret;
}

int ObUnitManager::get_unit_by_id(const uint64_t unit_id, ObUnit *&unit) const
{
  int ret = OB_SUCCESS;
  unit = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == unit_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(unit_id), K(ret));
  } else {
    GET_ITEM_FROM_MAP(id_unit_map_, unit_id, unit);
  }
  return ret;
}

int ObUnitManager::get_loads_by_server(const ObAddr &addr,
                                       ObArray<ObUnitManager::ObUnitLoad> *&loads) const
{
  int ret = OB_SUCCESS;
  loads = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!addr.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(addr), K(ret));
  } else {
    GET_ITEM_FROM_MAP(server_loads_, addr, loads);
  }
  return ret;
}

int ObUnitManager::get_tenant_ids_by_server(const ObAddr &addr, ObIArray<uint64_t> &tenant_ids)
{
  int ret = OB_SUCCESS;
  ObArray<ObUnitManager::ObUnitLoad> *loads = nullptr;
  tenant_ids.reuse();

  SpinRLockGuard guard(lock_);
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!addr.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(addr), K(ret));
  } else if (OB_FAIL(server_loads_.get_refactored(addr, loads))) {
    LOG_WARN("failed to get loads", K(ret), K(addr));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < loads->count(); ++i) {
      share::ObResourcePool *pool = loads->at(i).pool_;
      if (OB_ISNULL(pool)) {
        ret = OB_ERR_SYS;
        LOG_ERROR("pool must not null", K(ret), K(i), "count", loads->count());
      } else if (OB_FAIL(tenant_ids.push_back(pool->tenant_id_))) {
        LOG_WARN("failed to add tenant id", K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::get_pools_by_tenant(const uint64_t tenant_id,
                                       ObArray<share::ObResourcePool *> *&pools) const
{
  int ret = OB_SUCCESS;
  // meta tenant has no self resource pool, here return user tenant resource pool
  uint64_t valid_tnt_id = is_meta_tenant(tenant_id) ? gen_user_tenant_id(tenant_id) : tenant_id;

  pools = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(tenant_id), K(ret));
  } else if (OB_FAIL(get_pools_by_id(tenant_pools_map_, valid_tnt_id, pools))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_pools_by_id failed", K(valid_tnt_id), K(ret));
    }
  }
  return ret;
}

int ObUnitManager::get_pools_by_config(const uint64_t config_id,
                                       ObArray<share::ObResourcePool *> *&pools) const
{
  int ret = OB_SUCCESS;
  pools = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_INVALID_ID == config_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(config_id), K(ret));
  } else if (OB_FAIL(get_pools_by_id(config_pools_map_, config_id, pools))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_pools_by_id failed", K(config_id), K(ret));
    }
  }
  return ret;
}

int ObUnitManager::get_pools_by_id(
    const common::hash::ObHashMap<uint64_t, common::ObArray<share::ObResourcePool *> *> &map,
    const uint64_t id, ObArray<share::ObResourcePool *> *&pools) const
{
  int ret = OB_SUCCESS;
  pools = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!map.created() || OB_INVALID_ID == id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", "map created", map.created(), K(id), K(ret));
  } else {
    GET_ITEM_FROM_MAP(map, id, pools);
  }
  return ret;
}

int ObUnitManager::inner_check_single_logonly_pool_for_locality(
    const share::ObResourcePool &pool,
    const common::ObIArray<share::ObZoneReplicaAttrSet> &zone_locality,
    bool &is_legal)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObUnitManager not init", K(ret), K(inited_));
  } else if (REPLICA_TYPE_LOGONLY != pool.replica_type_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool replica type unexpected", K(ret), K(pool));
  } else {
    is_legal = true;
    for (int64_t i = 0; is_legal && i < pool.zone_list_.count(); ++i) {
      const common::ObZone &zone = pool.zone_list_.at(i);
      for (int64_t j = 0; is_legal && j < zone_locality.count(); ++j) {
        const common::ObIArray<common::ObZone> &zone_set = zone_locality.at(j).zone_set_;
        if (zone_set.count() <= 1) {
          // bypass Non-mixed locality
        } else { // mixed locality
          is_legal = !has_exist_in_array(zone_set, zone);
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_check_logonly_pools_for_locality(
    const common::ObIArray<share::ObResourcePool *> &pools,
    const common::ObIArray<share::ObZoneReplicaAttrSet> &zone_locality,
    bool &is_legal)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObUnitManager not init", K(ret), K(inited_));
  } else if (OB_UNLIKELY(pools.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguemnt", K(ret), "pool_count", pools.count());
  } else {
    is_legal = true;
    for (int64_t i = 0; is_legal && OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool = pools.at(i);
      if (OB_UNLIKELY(nullptr == pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (REPLICA_TYPE_LOGONLY != pool->replica_type_) {
        // bypass, since this is not logonly pool
      } else if (OB_FAIL(inner_check_single_logonly_pool_for_locality(
              *pool, zone_locality, is_legal))) {
        LOG_WARN("fail to inner check single logonly pool for locality", K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_check_pools_unit_num_enough_for_locality(
    const common::ObIArray<share::ObResourcePool *> &pools,
    const common::ObIArray<common::ObZone> &schema_zone_list,
    const ZoneLocalityIArray &zone_locality,
    bool &is_enough)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObUnitManager not init", K(ret), K(inited_));
  } else if (pools.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(pools.count()));
  } else {
    common::ObArray<int64_t> unit_nums;
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool = pools.at(i);
      if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (OB_FAIL(unit_nums.push_back(pool->unit_count_))) {
        LOG_WARN("fail to push back", K(ret));
      } else {} // no more to do
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(do_check_shrink_granted_pool_allowed_by_locality(
            pools, schema_zone_list, zone_locality, unit_nums, is_enough))) {
      LOG_WARN("fail to check pools unit num enough for locality", K(ret));
    } else {} // no more to do
  }
  return ret;
}

// The legality check consists of two parts
// 1 In the locality of the mixed scene, there can be no logonly resource pool
// 2 Can the unit num of the resource pool fit all locality copies
int ObUnitManager::check_pools_unit_legality_for_locality(
    const common::ObIArray<share::ObResourcePoolName> &pools,
    const common::ObIArray<common::ObZone> &schema_zone_list,
    const ZoneLocalityIArray &zone_locality,
    bool &is_legal)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObUnitManager not init", K(ret), K(inited_));
  } else if (pools.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(pools.count()));
  } else {
    SpinRLockGuard guard(lock_);
    common::ObArray<share::ObResourcePool *> pool_ptrs;
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      share::ObResourcePool *pool_ptr = NULL;
      if (OB_FAIL(inner_get_resource_pool_by_name(pools.at(i), pool_ptr))) {
        LOG_WARN("fail to get resource pool by name", K(ret), "pool_name", pools.at(i));
      } else if (OB_FAIL(pool_ptrs.push_back(pool_ptr))) {
        LOG_WARN("fail to push back", K(ret));
      } else {} // no more to do
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(inner_check_logonly_pools_for_locality(
            pool_ptrs, zone_locality, is_legal))) {
      LOG_WARN("fail to check logonly pools for locality", K(ret));
    } else if (!is_legal) {
      // no need to check any more
    } else if (OB_FAIL(inner_check_pools_unit_num_enough_for_locality(
            pool_ptrs, schema_zone_list, zone_locality, is_legal))) {
      LOG_WARN("fail to check pools unit num enough for locality", K(ret));
    } else {} // no more to do
  }
  return ret;
}

int ObUnitManager::get_migrate_units_by_server(const ObAddr &server,
                                               common::ObIArray<uint64_t> &migrate_units) const
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> *units= NULL;
  if (OB_FAIL(get_migrate_units_by_server(server, units))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS; // no migrating units
    } else {
      LOG_WARN("fail get migrate units by server", K(server), K(ret));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
      const uint64_t unit_id = units->at(i);
      if (OB_FAIL(migrate_units.push_back(unit_id))) {
        LOG_WARN("fail push back unit id to array", K(unit_id), K(i), K(ret));
      }
    }
  }
  return ret;
}

int ObUnitManager::get_migrate_units_by_server(const ObAddr &server,
                                               common::ObArray<uint64_t> *&migrate_units) const
{
  int ret = OB_SUCCESS;
  migrate_units = NULL;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (!server.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid server", K(server), K(ret));
  } else {
    GET_ITEM_FROM_MAP(server_migrate_units_map_, server, migrate_units);
  }
  return ret;
}

#undef DELETE_ITEM_FROM_MAP

int ObUnitManager::fetch_new_unit_config_id(uint64_t &config_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    ObMaxIdFetcher id_fetcher(*proxy_);
    uint64_t combine_id = OB_INVALID_ID;
    if (OB_FAIL(id_fetcher.fetch_new_max_id(
        OB_SYS_TENANT_ID, OB_MAX_USED_UNIT_CONFIG_ID_TYPE, combine_id))) {
      LOG_WARN("fetch_max_id failed", "id_type", OB_MAX_USED_UNIT_CONFIG_ID_TYPE, K(ret));
    } else {
      config_id = combine_id;
    }
  }
  return ret;
}

int ObUnitManager::fetch_new_resource_pool_id(uint64_t &resource_pool_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    uint64_t combine_id = OB_INVALID_ID;
    ObMaxIdFetcher id_fetcher(*proxy_);
    if (OB_FAIL(id_fetcher.fetch_new_max_id(OB_SYS_TENANT_ID,
        OB_MAX_USED_RESOURCE_POOL_ID_TYPE, combine_id))) {
      LOG_WARN("fetch_new_max_id failed", "id_type", OB_MAX_USED_RESOURCE_POOL_ID_TYPE, K(ret));
    } else {
      resource_pool_id = combine_id;
    }
  }
  return ret;
}

int ObUnitManager::fetch_new_unit_group_id(uint64_t &unit_group_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    uint64_t combine_id = OB_INVALID_ID;
    ObMaxIdFetcher id_fetcher(*proxy_);
    if (OB_FAIL(id_fetcher.fetch_new_max_id(OB_SYS_TENANT_ID,
        OB_MAX_USED_UNIT_GROUP_ID_TYPE, combine_id))) {
      LOG_WARN("fetch_new_max_id failed", "id_type", OB_MAX_USED_UNIT_ID_TYPE, K(ret));
    } else {
      unit_group_id = combine_id;
    }
  }
  return ret;
}

int ObUnitManager::fetch_new_unit_id(uint64_t &unit_id)
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    uint64_t combine_id = OB_INVALID_ID;
    ObMaxIdFetcher id_fetcher(*proxy_);
    if (OB_FAIL(id_fetcher.fetch_new_max_id(OB_SYS_TENANT_ID,
        OB_MAX_USED_UNIT_ID_TYPE, combine_id))) {
      LOG_WARN("fetch_new_max_id failed", "id_type", OB_MAX_USED_UNIT_ID_TYPE, K(ret));
    } else {
      unit_id = combine_id;
    }
  }
  return ret;
}

int ObUnitManager::check_bootstrap_pool(const share::ObResourcePool &pool)
{
  int ret = OB_SUCCESS;
  if (!pool.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid pool", K(pool), K(ret));
  } else if (ObUnitConfig::SYS_UNIT_CONFIG_ID == pool.unit_config_id_) {
    // good
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("pool not sys pool", K(pool), K(ret));
  }
  return ret;
}

int ObUnitManager::get_tenant_zone_full_unit_num(
    const int64_t tenant_id,
    const common::ObZone &zone,
    int64_t &unit_num)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(inner_get_tenant_zone_full_unit_num(
          tenant_id, zone, unit_num))) {
    LOG_WARN("fail to inner get tenant zone full unit num", KR(ret), K(tenant_id), K(zone));
  }
  return ret;
}

int ObUnitManager::inner_get_tenant_zone_full_unit_num(
    const int64_t tenant_id,
    const common::ObZone &zone,
    int64_t &unit_num)
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *pools = NULL;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(NULL == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools ptr is null", K(ret), KP(pools));
  } else {
    bool find = false;
    for (int64_t i = 0; !find && OB_SUCC(ret) && i < pools->count(); ++i) {
      share::ObResourcePool *pool = pools->at(i);
      if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (!has_exist_in_array(pool->zone_list_, zone)) {
        // not in this pool
      } else if (REPLICA_TYPE_FULL == pool->replica_type_) {
        unit_num = pool->unit_count_;
        find = true;
      } else {} // not a full replica type resource pool, go on to check next
    }
    if (OB_FAIL(ret)) {
    } else if (!find) {
      unit_num = 0;
    }
  }
  return ret;
}

int ObUnitManager::get_tenant_zone_unit_loads(
    const int64_t tenant_id,
    const common::ObZone &zone,
    const common::ObReplicaType replica_type,
    common::ObIArray<ObUnitManager::ObUnitLoad> &unit_loads)
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *pools = NULL;
  unit_loads.reset();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id)
                         || zone.is_empty()
                         || !ObReplicaTypeCheck::is_replica_type_valid(replica_type))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(zone), K(replica_type));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
    }
  } else if (OB_UNLIKELY(NULL == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool ptr is null", K(ret), KP(pools));
  } else {
    unit_loads.reset();
    share::ObResourcePool *target_pool = NULL;
    for (int64_t i = 0; NULL == target_pool && OB_SUCC(ret) && i < pools->count(); ++i) {
      share::ObResourcePool *pool = pools->at(i);
      if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (!has_exist_in_array(pool->zone_list_, zone)) {
        // not in this pool
      } else if (replica_type == pool->replica_type_) {
        target_pool = pool;
      } else {} // not a full replica type resource pool, go on to check next
    }
    if (OB_FAIL(ret)) {
    } else if (NULL == target_pool) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      ObArray<share::ObUnit *> *units = NULL;
      if (OB_FAIL(get_units_by_pool(target_pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", K(ret),
                 "pool_id", target_pool->resource_pool_id_);
      } else if (OB_UNLIKELY(NULL == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptr is null", K(ret), KP(units));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
          ObUnitLoad unit_load;
          ObUnit *unit = units->at(i);
          if (NULL == unit) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", K(ret));
          } else if (unit->zone_ != zone) {
            // not this zone, ignore
          } else if (OB_FAIL(gen_unit_load(unit, unit_load))) {
            LOG_WARN("fail to gen unit load", K(ret));
          } else if (OB_FAIL(unit_loads.push_back(unit_load))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_tenant_zone_all_unit_loads(
    const int64_t tenant_id,
    const common::ObZone &zone,
    common::ObIArray<ObUnitManager::ObUnitLoad> &unit_loads)
{
  int ret = OB_SUCCESS;
  ObArray<share::ObResourcePool *> *pools = NULL;
  unit_loads.reset();
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || zone.is_empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(zone));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to get pools by tenant", K(ret), K(tenant_id));
    }
  } else if (OB_UNLIKELY(NULL == pools)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pool ptr is null", K(ret), KP(pools));
  } else {
    unit_loads.reset();
    common::ObArray<share::ObResourcePool *> target_pools;
    for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
      share::ObResourcePool *pool = pools->at(i);
      if (NULL == pool) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool ptr is null", K(ret), KP(pool));
      } else if (!has_exist_in_array(pool->zone_list_, zone)) {
        // not in this pool
      } else if (OB_FAIL(target_pools.push_back(pool))) {
        LOG_WARN("fail to push back", K(ret));
      } else {} // no more to do
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < target_pools.count(); ++i) {
      ObArray<share::ObUnit *> *units = NULL;
      share::ObResourcePool *target_pool = target_pools.at(i);
      if (OB_UNLIKELY(NULL == target_pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("target pool ptr is null", K(ret), KP(target_pool));
      } else if (OB_FAIL(get_units_by_pool(target_pool->resource_pool_id_, units))) {
        LOG_WARN("fail to get units by pool", K(ret),
                 "pool_id", target_pool->resource_pool_id_);
      } else if (OB_UNLIKELY(NULL == units)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("units ptr is null", K(ret), KP(units));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < units->count(); ++i) {
          ObUnitLoad unit_load;
          ObUnit *unit = units->at(i);
          if (NULL == unit) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unit ptr is null", K(ret));
          } else if (unit->zone_ != zone) {
            // not this zone, ignore
          } else if (OB_FAIL(gen_unit_load(unit, unit_load))) {
            LOG_WARN("fail to gen unit load", K(ret));
          } else if (OB_FAIL(unit_loads.push_back(unit_load))) {
            LOG_WARN("fail to push back", K(ret));
          } else {} // no more to do
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (unit_loads.count() <= 0) {
      ret = OB_ENTRY_NOT_EXIST;
    }
  }
  return ret;
}

int ObUnitManager::try_renotify_server_unit_info_for_fast_recovery(
    const common::ObAddr &server,
    const common::ObZone &zone)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObResourcePool> pools;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(ret), K(inited_), K(loaded_));
  } else if (OB_UNLIKELY(!server.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_FAIL(get_pools(pools))) {
    LOG_WARN("fail to get pools", K(ret));
  } else {
    common::ObArray<share::ObUnitInfo> related_unit_infos;
    common::ObArray<share::ObUnitInfo> my_unit_infos;
    for (int64_t i = 0; OB_SUCC(ret) && i < pools.count(); ++i) {
      const share::ObResourcePool &pool = pools.at(i);
      if (OB_FAIL(get_unit_infos_of_pool(pool.resource_pool_id_, my_unit_infos))) {
        LOG_WARN("fail to get unit infos of pool", K(ret), K(pool));
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < my_unit_infos.count(); ++j) {
          const share::ObUnitInfo &tui = my_unit_infos.at(j);
          if (tui.unit_.server_ == server || tui.unit_.migrate_from_server_ == server) {
            if (OB_FAIL(related_unit_infos.push_back(tui))) {
              LOG_WARN("fail to push back", K(ret));
            }
          } else {
            // not on this server, ignore
          }
        }
      }
    }

    ObNotifyTenantServerResourceProxy notify_proxy(
                                      *srv_rpc_proxy_,
                                      &obrpc::ObSrvRpcProxy::notify_tenant_server_unit_resource);
    ObArray<int> return_code_array;
    for (int64_t i = 0; OB_SUCC(ret) && i < related_unit_infos.count(); ++i) {
      const share::ObUnitInfo &unit_info = related_unit_infos.at(i);
      const uint64_t tenant_id = unit_info.pool_.tenant_id_;
      lib::Worker::CompatMode compat_mode = lib::Worker::CompatMode::INVALID;
      if (!is_valid_tenant_id(tenant_id)) {
        // 跳过，不属于任何租户
      } else if (OB_FAIL(ObCompatModeGetter::get_tenant_mode(tenant_id, compat_mode))) {
        LOG_WARN("fail to get tenant compat mode", K(ret));
      } else {
        const int64_t start = ObTimeUtility::current_time();
        obrpc::TenantServerUnitConfig tenant_unit_server_config;
        tenant_unit_server_config.tenant_id_ = tenant_id;
        tenant_unit_server_config.compat_mode_ = compat_mode;
        tenant_unit_server_config.unit_config_ = unit_info.config_;
        tenant_unit_server_config.replica_type_ = unit_info.unit_.replica_type_;
        tenant_unit_server_config.if_not_grant_ = false;
        tenant_unit_server_config.is_delete_ = false;
        int64_t rpc_timeout = NOTIFY_RESOURCE_RPC_TIMEOUT;

        if (OB_FAIL(notify_proxy.call(
                unit_info.unit_.server_, rpc_timeout, tenant_unit_server_config))) {
          LOG_WARN("fail to call notify resource to server",
                   K(ret), K(rpc_timeout), "dst", unit_info.unit_.server_);
        } else {
          LOG_INFO("call notify resource to server", K(ret),
                   "dst", unit_info.unit_.server_,
                   "cost", ObTimeUtility::current_time() - start);
        }

        if (OB_FAIL(ret)) {
          // failed
        } else if (!unit_info.unit_.migrate_from_server_.is_valid()) {

        } else if (OB_FAIL(notify_proxy.call(
                unit_info.unit_.migrate_from_server_, rpc_timeout, tenant_unit_server_config))) {
          LOG_WARN("fail to call notify resource to server",
                   K(ret), K(rpc_timeout), "dst", unit_info.unit_.migrate_from_server_);
        } else {
          LOG_INFO("call notify resource to server", K(ret),
                   "dst", unit_info.unit_.migrate_from_server_,
                   "cost", ObTimeUtility::current_time() - start);
        }
      }
    }
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = notify_proxy.wait_all(return_code_array))) {
      LOG_WARN("wait batch result failed", K(ret), K(tmp_ret));
      ret = (OB_SUCCESS == ret) ? tmp_ret : ret;
    }
    if (OB_FAIL(ret)) {
      // bypass
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < return_code_array.count(); ++i) {
        int this_ret = return_code_array.at(i);
        if (OB_SUCCESS == this_ret || OB_TENANT_EXIST == this_ret) {
          // good
        } else {
          ret = this_ret;
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_logonly_unit_ids(ObIArray<uint64_t> &unit_ids) const
{
  int ret = OB_SUCCESS;
  unit_ids.reset();
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  }
  for (ObHashMap<uint64_t, ObArray<ObUnit *> *>::const_iterator it = pool_unit_map_.begin();
      OB_SUCCESS == ret && it != pool_unit_map_.end(); ++it) {
    if (NULL == it->second) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("pointer of ObArray<ObUnit *> is null", "pool_id", it->first, K(ret));
    } else if (it->second->count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("array of unit is empty", "pool_id", it->first, K(ret));
    } else {
      const ObArray<ObUnit *> units = *it->second;
      for (int64_t i = 0; OB_SUCC(ret) && i < units.count(); ++i) {
        uint64_t unit_id = units.at(i)->unit_id_;
        if (REPLICA_TYPE_LOGONLY != units.at(i)->replica_type_) {
          //nothing todo
        } else if (OB_FAIL(unit_ids.push_back(unit_id))) {
          LOG_WARN("fail push back it", K(unit_id), K(ret));
        } else {
          LOG_WARN("get logonly unit ids", K(unit_id));
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::get_unit_info_by_id(const uint64_t unit_id, share::ObUnitInfo &unit_info) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("fail to check inner stat", K(ret), K(inited_), K(loaded_));
  } else if (OB_FAIL(inner_get_unit_info_by_id(unit_id, unit_info))) {
    LOG_WARN("fail to get unit info by id", K(ret), K(unit_id));
  }
  return ret;
}

int ObUnitManager::get_logonly_unit_by_tenant(const int64_t tenant_id,
                                              ObIArray<ObUnitInfo> &logonly_unit_infos)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  if (OB_ISNULL(schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service is null", K(schema_service_), KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(tenant_id, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(tenant_id));
  } else if (OB_FAIL(get_logonly_unit_by_tenant(schema_guard, tenant_id, logonly_unit_infos))) {
    LOG_WARN("get logonly unit by tenant fail", KR(ret), K(tenant_id));
  }
  return ret;
}

int ObUnitManager::get_logonly_unit_by_tenant(share::schema::ObSchemaGetterGuard &schema_guard,
                                              const int64_t tenant_id,
                                              ObIArray<ObUnitInfo> &logonly_unit_infos)
{
  int ret = OB_SUCCESS;
  const ObTenantSchema *tenant_schema = NULL;
  logonly_unit_infos.reset();
  ObArray<ObUnitInfo> unit_infos;
  SpinRLockGuard guard(lock_);
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), K(tenant_id));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get invalid tenant_schema", K(ret), K(tenant_id));
  } else if (OB_FAIL(inner_get_active_unit_infos_of_tenant(*tenant_schema, unit_infos))) {
    LOG_WARN("fail to get active unit", K(ret), K(tenant_id));
  } else {
    for (int64_t i = 0; i < unit_infos.count() && OB_SUCC(ret); i++) {
      if (REPLICA_TYPE_LOGONLY != unit_infos.at(i).unit_.replica_type_) {
        //nothing todo
      } else if (OB_FAIL(logonly_unit_infos.push_back(unit_infos.at(i)))) {
        LOG_WARN("fail to push back", K(ret), K(unit_infos));
      }
    }
  }
  return ret;
}

int ObUnitManager::get_unit_infos(const common::ObIArray<share::ObResourcePoolName> &pools,
                                  ObIArray<ObUnitInfo> &unit_infos)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  unit_infos.reset();
  if (pools.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(pools));
  } else {
    ObArray<ObUnitInfo> pool_units;
    share::ObResourcePool *pool = NULL;
    for (int64_t i = 0; i < pools.count() && OB_SUCC(ret); i++) {
      pool_units.reset();
      if (OB_FAIL(inner_get_resource_pool_by_name(pools.at(i), pool))) {
        LOG_WARN("fail to get resource pool", K(ret), K(i), K(pools));
      } else if (OB_ISNULL(pool)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get invalid pool", K(ret), K(pools));
      } else if (OB_FAIL(inner_get_unit_infos_of_pool(pool->resource_pool_id_, pool_units))) {
        LOG_WARN("fail to get unit infos", K(ret), K(*pool));
      } else {
        for (int64_t j = 0; j < pool_units.count() && OB_SUCC(ret); j++) {
          if (OB_FAIL(unit_infos.push_back(pool_units.at(j)))) {
            LOG_WARN("fail to push back", K(ret), K(pool_units));
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_get_active_unit_infos_of_tenant(const ObTenantSchema &tenant_schema,
                                                  ObIArray<ObUnitInfo> &unit_info)
{
  int ret = OB_SUCCESS;
  ObArray<uint64_t> pool_ids;
  uint64_t tenant_id = tenant_schema.get_tenant_id();
  common::ObArray<common::ObZone> tenant_zone_list;
  ObArray<share::ObResourcePool  *> *pools = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (OB_FAIL(tenant_schema.get_zone_list(tenant_zone_list))) {
    LOG_WARN("fail to get zone list", K(ret));
  } else if (OB_FAIL(get_pools_by_tenant(tenant_id, pools))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("get_pools_by_tenant failed", K(tenant_id), K(ret));
    } else {
      // just return empty pool_ids
      ret = OB_SUCCESS;
      LOG_WARN("tenant doesn't own any pool", K(tenant_id), K(ret));
    }
  } else if (NULL == pools) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("pools is null", KP(pools), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < pools->count(); ++i) {
      if (NULL == pools->at(i)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pool is null", "pool", OB_P(pools->at(i)), K(ret));
      } else if (OB_FAIL(pool_ids.push_back(pools->at(i)->resource_pool_id_))) {
        LOG_WARN("push_back failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObArray<ObUnitInfo> unit_in_pool;
    for (int64_t i = 0; i < pool_ids.count() && OB_SUCC(ret); i++) {
      uint64_t pool_id = pool_ids.at(i);
      unit_in_pool.reset();
      if (OB_FAIL(inner_get_unit_infos_of_pool(pool_id, unit_in_pool))) {
        LOG_WARN("fail to inner get unit infos", K(ret), K(pool_id));
      } else {
        for (int64_t j = 0; j < unit_in_pool.count() && OB_SUCC(ret); j++) {
          if (ObUnit::UNIT_STATUS_ACTIVE != unit_in_pool.at(j).unit_.status_) {
            //nothing todo
          } else if (!has_exist_in_array(tenant_zone_list, unit_in_pool.at(j).unit_.zone_)) {
            //nothing todo
          } else if (OB_FAIL(unit_info.push_back(unit_in_pool.at(j)))) {
            LOG_WARN("fail to push back", K(ret), K(unit_in_pool), K(j));
          }
        }
      }
    }
  }
  return ret;
}

int ObUnitManager::inner_get_active_unit_infos_of_tenant(const uint64_t tenant_id,
                                                  ObIArray<ObUnitInfo> &unit_info)
{
  int ret = OB_SUCCESS;
  unit_info.reset();
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTenantSchema *tenant_schema = NULL;
  if (!check_inner_stat()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("check_inner_stat failed", K(inited_), K(loaded_), K(ret));
  } else if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id));
  } else if (OB_ISNULL(schema_service_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("schema service is null", K(schema_service_), KR(ret));
  } else if (OB_FAIL(schema_service_->get_tenant_schema_guard(tenant_id, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), K(tenant_id));
  } else if (OB_UNLIKELY(NULL == tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant schema is null", K(ret), KP(tenant_schema));
  } else if (OB_FAIL(inner_get_active_unit_infos_of_tenant(*tenant_schema, unit_info))) {
    LOG_WARN("fail to get active unit infos", K(ret), K(tenant_id));
  }
    return ret;
}

}//end namespace share
}//end namespace oceanbase
