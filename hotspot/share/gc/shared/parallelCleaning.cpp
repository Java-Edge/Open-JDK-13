/*
 * Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/stringTable.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/parallelCleaning.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "logging/log.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#endif

StringDedupCleaningTask::StringDedupCleaningTask(BoolObjectClosure* is_alive,
                                                 OopClosure* keep_alive,
                                                 bool resize_table) :
  AbstractGangTask("String Dedup Cleaning"),
  _dedup_closure(is_alive, keep_alive) {

  if (StringDedup::is_enabled()) {
    StringDedup::gc_prologue(resize_table);
  }
}

StringDedupCleaningTask::~StringDedupCleaningTask() {
  if (StringDedup::is_enabled()) {
    StringDedup::gc_epilogue();
  }
}

void StringDedupCleaningTask::work(uint worker_id) {
  if (StringDedup::is_enabled()) {
    StringDedup::parallel_unlink(&_dedup_closure, worker_id);
  }
}

CodeCacheUnloadingTask::CodeCacheUnloadingTask(uint num_workers, BoolObjectClosure* is_alive, bool unloading_occurred) :
  _unloading_scope(is_alive),
  _unloading_occurred(unloading_occurred),
  _num_workers(num_workers),
  _first_nmethod(NULL),
  _claimed_nmethod(NULL) {
  // Get first alive nmethod
  CompiledMethodIterator iter(CompiledMethodIterator::only_alive);
  if(iter.next()) {
    _first_nmethod = iter.method();
  }
  _claimed_nmethod = _first_nmethod;
}

CodeCacheUnloadingTask::~CodeCacheUnloadingTask() {
  CodeCache::verify_clean_inline_caches();
  CodeCache::verify_icholder_relocations();
}

void CodeCacheUnloadingTask::claim_nmethods(CompiledMethod** claimed_nmethods, int *num_claimed_nmethods) {
  CompiledMethod* first;
  CompiledMethodIterator last(CompiledMethodIterator::only_alive);

  do {
    *num_claimed_nmethods = 0;

    first = _claimed_nmethod;
    last = CompiledMethodIterator(CompiledMethodIterator::only_alive, first);

    if (first != NULL) {

      for (int i = 0; i < MaxClaimNmethods; i++) {
        if (!last.next()) {
          break;
        }
        claimed_nmethods[i] = last.method();
        (*num_claimed_nmethods)++;
      }
    }

  } while (Atomic::cmpxchg(last.method(), &_claimed_nmethod, first) != first);
}

void CodeCacheUnloadingTask::work(uint worker_id) {
  // The first nmethods is claimed by the first worker.
  if (worker_id == 0 && _first_nmethod != NULL) {
    _first_nmethod->do_unloading(_unloading_occurred);
    _first_nmethod = NULL;
  }

  int num_claimed_nmethods;
  CompiledMethod* claimed_nmethods[MaxClaimNmethods];

  while (true) {
    claim_nmethods(claimed_nmethods, &num_claimed_nmethods);

    if (num_claimed_nmethods == 0) {
      break;
    }

    for (int i = 0; i < num_claimed_nmethods; i++) {
      claimed_nmethods[i]->do_unloading(_unloading_occurred);
    }
  }
}

KlassCleaningTask::KlassCleaningTask() :
  _clean_klass_tree_claimed(0),
  _klass_iterator() {
}

bool KlassCleaningTask::claim_clean_klass_tree_task() {
  if (_clean_klass_tree_claimed) {
    return false;
  }

  return Atomic::cmpxchg(1, &_clean_klass_tree_claimed, 0) == 0;
}

InstanceKlass* KlassCleaningTask::claim_next_klass() {
  Klass* klass;
  do {
    klass =_klass_iterator.next_klass();
  } while (klass != NULL && !klass->is_instance_klass());

  // this can be null so don't call InstanceKlass::cast
  return static_cast<InstanceKlass*>(klass);
}

void KlassCleaningTask::work() {
  ResourceMark rm;

  // One worker will clean the subklass/sibling klass tree.
  if (claim_clean_klass_tree_task()) {
    Klass::clean_subklass_tree();
  }

  // All workers will help cleaning the classes,
  InstanceKlass* klass;
  while ((klass = claim_next_klass()) != NULL) {
    clean_klass(klass);
  }
}

#if INCLUDE_JVMCI
JVMCICleaningTask::JVMCICleaningTask() :
  _cleaning_claimed(0) {
}

bool JVMCICleaningTask::claim_cleaning_task() {
  if (_cleaning_claimed) {
    return false;
  }

  return Atomic::cmpxchg(1, &_cleaning_claimed, 0) == 0;
}

void JVMCICleaningTask::work(bool unloading_occurred) {
  // One worker will clean JVMCI metadata handles.
  if (unloading_occurred && EnableJVMCI && claim_cleaning_task()) {
    JVMCI::do_unloading(unloading_occurred);
  }
}
#endif // INCLUDE_JVMCI

ParallelCleaningTask::ParallelCleaningTask(BoolObjectClosure* is_alive,
                                           uint num_workers,
                                           bool unloading_occurred,
                                           bool resize_dedup_table) :
  AbstractGangTask("Parallel Cleaning"),
  _unloading_occurred(unloading_occurred),
  _string_dedup_task(is_alive, NULL, resize_dedup_table),
  _code_cache_task(num_workers, is_alive, unloading_occurred),
  JVMCI_ONLY(_jvmci_cleaning_task() COMMA)
  _klass_cleaning_task() {
}

// The parallel work done by all worker threads.
void ParallelCleaningTask::work(uint worker_id) {
  // Clean JVMCI metadata handles.
  // Execute this task first because it is serial task.
  JVMCI_ONLY(_jvmci_cleaning_task.work(_unloading_occurred);)

  // Do first pass of code cache cleaning.
  _code_cache_task.work(worker_id);

  // Clean the string dedup data structures.
  _string_dedup_task.work(worker_id);

  // Clean all klasses that were not unloaded.
  // The weak metadata in klass doesn't need to be
  // processed if there was no unloading.
  if (_unloading_occurred) {
    _klass_cleaning_task.work();
  }
}