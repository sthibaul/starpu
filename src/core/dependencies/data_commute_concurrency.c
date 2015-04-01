/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2015  Université de Bordeaux
 * Copyright (C) 2015  Inria
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <core/dependencies/data_concurrency.h>
#include <datawizard/coherency.h>
#include <core/sched_policy.h>
#include <common/starpu_spinlock.h>
#include <datawizard/sort_data_handles.h>

//#define LOCK_OR_DELEGATE

/*
 * This implements a solution for the dining philosophers problem (see
 * data_concurrency.c for the rationale) based on a centralized arbiter.  This
 * allows to get a more parallel solution than the Dijkstra solution, by
 * avoiding strictly serialized executions, and instead opportunistically find
 * which tasks can take data.
 *
 * These are the algorithms implemented below:
 *
 *
 * at termination of task T:
 *
 * - for each handle h of T:
 *   - mutex_lock(&arbiter)
 *   - release reference on h
 *   - for each task Tc waiting for h:
 *     - for each data Tc_h it is waiting:
 *       - if Tc_h is busy, goto fail
 *     // Ok, now really take them
 *     - For each data Tc_h it is waiting:
 *       - lock(Tc_h)
 *       - take reference on h (it should be still available since we hold the arbiter)
 *       - unlock(Tc_h)
 *     // Ok, we managed to find somebody, we're finished!
 *     _starpu_push_task(Tc);
 *     break;
 *     fail:
 *       // No luck, let's try another task
 *       continue;
 *   // Release the arbiter mutex a bit from time to time
 *   - mutex_unlock(&arbiter)
 *
 *
 * at submission of task T:
 *
 * - mutex_lock(&arbiter)
 * - for each handle h of T:
 *   - lock(h)
 *   - try to take a reference on h, goto fail on failure
 *   - unlock(h)
 * // Success!
 * - mutex_unlock(&arbiter);
 * - return 0;
 *
 * fail:
 * // couldn't take everything, abort and record task T
 * // drop spurious references
 * - for each handle h of T already taken:
 *   - lock(h)
 *   - release reference on h
 *   - unlock(h)
 * // record T on the list of requests for h
 * - for each handle h of T:
 *   - record T as waiting on h
 * - mutex_unlock(&arbiter)
 * - return 1;
 */

struct starpu_arbiter
{
#ifdef LOCK_OR_DELEGATE
/* The list of task to perform */
	struct LockOrDelegateListNode* dlTaskListHead;

/* To protect the list of tasks */
	struct _starpu_spinlock dlListLock;
/* Whether somebody is working on the list */
	int working;
#else /* LOCK_OR_DELEGATE */
	starpu_pthread_mutex_t mutex;
#endif /* LOCK_OR_DELEGATE */
};

#ifdef LOCK_OR_DELEGATE

/* In case of congestion, we don't want to needlessly wait for the arbiter lock
 * while we can just delegate the work to the worker already managing some
 * dependencies.
 *
 * So we push work on the dlTastListHead queue and only one worker will process
 * the list.
 */

/* A LockOrDelegate task list */
struct LockOrDelegateListNode
{
	void (*func)(void*);
	void* data;
	struct LockOrDelegateListNode* next;
};

/* Post a task to perfom if possible, otherwise put it in the list
 * If we can perfom this task, we may also perfom all the tasks in the list
 * This function return 1 if the task (and maybe some others) has been done
 * by the calling thread and 0 otherwise (if the task has just been put in the list)
 */
static int _starpu_LockOrDelegatePostOrPerform(starpu_arbiter_t arbiter, void (*func)(void*), void* data)
{
	struct LockOrDelegateListNode* newNode = malloc(sizeof(*newNode)), *iter;
	int did = 0;
	STARPU_ASSERT(newNode);
	newNode->data = data;
	newNode->func = func;

	_starpu_spin_lock(&arbiter->dlListLock);
	if (arbiter->working)
	{
		/* Somebody working on it, insert the node */
		newNode->next = arbiter->dlTaskListHead;
		arbiter->dlTaskListHead = newNode;
	}
	else
	{
		/* Nobody working on the list, we'll work */
		arbiter->working = 1;

		/* work on what was pushed so far first */
		iter = arbiter->dlTaskListHead;
		arbiter->dlTaskListHead = NULL;
		_starpu_spin_unlock(&arbiter->dlListLock);
		while (iter != NULL)
		{
			(*iter->func)(iter->data);
			free(iter);
			iter = iter->next;
		}

		/* And then do our job */
		(*func)(data);
		free(newNode);
		did = 1;

		_starpu_spin_lock(&arbiter->dlListLock);
		/* And finish working on anything that could have been pushed
		 * in the meanwhile */
		while (arbiter->dlTaskListHead != 0)
		{
			iter = arbiter->dlTaskListHead;
			arbiter->dlTaskListHead = arbiter->dlTaskListHead->next;
			_starpu_spin_unlock(&arbiter->dlListLock);

			(*iter->func)(iter->data);
			free(iter);
			_starpu_spin_lock(&arbiter->dlListLock);
		}

		arbiter->working = 0;
	}

	_starpu_spin_unlock(&arbiter->dlListLock);
	return did;
}

#endif


/* This function find a node that contains the parameter j as job and remove it from the list
 * the function return 0 if a node was found and deleted, 1 otherwise
 */
static unsigned remove_job_from_requester_list(struct _starpu_data_requester_list* req_list, struct _starpu_job * j)
{
	struct _starpu_data_requester * iter = _starpu_data_requester_list_begin(req_list);//_head;
	while (iter != _starpu_data_requester_list_end(req_list) && iter->j != j)
	{
		iter = _starpu_data_requester_list_next(iter); // iter = iter->_next;
	}
	if (iter)
	{
		_starpu_data_requester_list_erase(req_list, iter);
		return 0;
	}
	return 1;
}

#ifdef LOCK_OR_DELEGATE
/* These are the arguments passed to _submit_job_enforce_arbitered_deps */
struct starpu_enforce_arbitered_args
{
	struct _starpu_job *j;
	unsigned buf;
	unsigned nbuffers;
};

static void ___starpu_submit_job_enforce_arbitered_deps(struct _starpu_job *j, unsigned buf, unsigned nbuffers);
static void __starpu_submit_job_enforce_arbitered_deps(void* inData)
{
	struct starpu_enforce_arbitered_args* args = (struct starpu_enforce_arbitered_args*)inData;
	struct _starpu_job *j = args->j;
	unsigned buf		  = args->buf;
	unsigned nbuffers	 = args->nbuffers;
	/* we are in charge of freeing the args */
	free(args);
	args = NULL;
	inData = NULL;
	___starpu_submit_job_enforce_arbitered_deps(j, buf, nbuffers);
}

void _starpu_submit_job_enforce_arbitered_deps(struct _starpu_job *j, unsigned buf, unsigned nbuffers)
{
	struct starpu_enforce_arbitered_args* args = malloc(sizeof(*args));
	starpu_data_handle_t handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, buf);
	args->j = j;
	args->buf = buf;
	args->nbuffers = nbuffers;
	/* The function will delete args */
	_starpu_LockOrDelegatePostOrPerform(handle->arbiter, &__starpu_submit_job_enforce_arbitered_deps, args);
}

static void ___starpu_submit_job_enforce_arbitered_deps(struct _starpu_job *j, unsigned buf, unsigned nbuffers)
{
	starpu_arbiter_t arbiter = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, buf)->arbiter;
#else // LOCK_OR_DELEGATE
void _starpu_submit_job_enforce_arbitered_deps(struct _starpu_job *j, unsigned buf, unsigned nbuffers)
{
	starpu_arbiter_t arbiter = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, buf)->arbiter;
	STARPU_PTHREAD_MUTEX_LOCK(&arbiter->mutex);
#endif
	STARPU_ASSERT(arbiter);

	const unsigned nb_non_arbitered_buff = buf;
	unsigned idx_buf_arbiter;
	unsigned all_arbiter_available = 1;


	for (idx_buf_arbiter = nb_non_arbitered_buff; idx_buf_arbiter < nbuffers; idx_buf_arbiter++)
	{
		starpu_data_handle_t handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_arbiter);
		enum starpu_data_access_mode mode = _STARPU_JOB_GET_ORDERED_BUFFER_MODE(j, idx_buf_arbiter);

		if (idx_buf_arbiter && (_STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_arbiter-1)==handle))
			/* We have already requested this data, skip it. This
			 * depends on ordering putting writes before reads, see
			 * _starpu_compar_handles.  */
			continue;

		if (handle->arbiter != arbiter)
		{
			/* another arbiter */
			break;
		}

		/* we post all arbiter  */
		_starpu_spin_lock(&handle->header_lock);
		if (handle->refcnt == 0)
		{
			handle->refcnt += 1;
			handle->busy_count += 1;
			handle->current_mode = mode;
			_starpu_spin_unlock(&handle->header_lock);
		}
		else
		{
			/* stop if an handle do not have a refcnt == 0 */
			_starpu_spin_unlock(&handle->header_lock);
			all_arbiter_available = 0;
			break;
		}
	}
	if (all_arbiter_available == 0)
	{
		/* Oups cancel all taken and put req in arbiter list */
		unsigned idx_buf_cancel;
		for (idx_buf_cancel = nb_non_arbitered_buff; idx_buf_cancel < idx_buf_arbiter ; idx_buf_cancel++)
		{
			starpu_data_handle_t cancel_handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_cancel);

			if (idx_buf_cancel && (_STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_cancel-1)==cancel_handle))
				continue;
			if (cancel_handle->arbiter != arbiter)
				/* Will have to process another arbiter, will do that later */
				break;

			_starpu_spin_lock(&cancel_handle->header_lock);
			/* reset the counter because finally we do not take the data */
			STARPU_ASSERT(cancel_handle->refcnt == 1);
			cancel_handle->refcnt -= 1;
			_starpu_spin_unlock(&cancel_handle->header_lock);
		}

		for (idx_buf_cancel = nb_non_arbitered_buff; idx_buf_cancel < nbuffers ; idx_buf_cancel++)
		{
			starpu_data_handle_t cancel_handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_cancel);
			enum starpu_data_access_mode cancel_mode = _STARPU_JOB_GET_ORDERED_BUFFER_MODE(j, idx_buf_cancel);

			if (cancel_handle->arbiter != arbiter)
				break;

			struct _starpu_data_requester *r = _starpu_data_requester_new();
			r->mode = cancel_mode;
			r->is_requested_by_codelet = 1;
			r->j = j;
			r->buffer_index = idx_buf_cancel;
			r->ready_data_callback = NULL;
			r->argcb = NULL;

			_starpu_spin_lock(&cancel_handle->header_lock);
			/* create list if needed */
			if (cancel_handle->arbitered_req_list == NULL)
				cancel_handle->arbitered_req_list = _starpu_data_requester_list_new();
			/* store node in list */
			_starpu_data_requester_list_push_front(cancel_handle->arbitered_req_list, r);
			/* inc the busy count if it has not been changed in the previous loop */
			if (idx_buf_arbiter <= idx_buf_cancel)
				cancel_handle->busy_count += 1;
			_starpu_spin_unlock(&cancel_handle->header_lock);
		}

#ifndef LOCK_OR_DELEGATE
		STARPU_PTHREAD_MUTEX_UNLOCK(&arbiter->mutex);
#endif
		return 1;
	}
#ifndef LOCK_OR_DELEGATE
	STARPU_PTHREAD_MUTEX_UNLOCK(&arbiter->mutex);
#endif

	// all_arbiter_available is true
	if (idx_buf_arbiter < nbuffers)
	{
		/* Other arbitered data, process them */
		return _starpu_submit_job_enforce_arbitered_deps(j, idx_buf_arbiter, nbuffers);
	}
	/* Finished with all data, can eventually push! */
	_starpu_push_task(j);
	return 0;
}

#ifdef LOCK_OR_DELEGATE
void ___starpu_notify_arbitered_dependencies(starpu_data_handle_t handle);
void __starpu_notify_arbitered_dependencies(void* inData)
{
	starpu_data_handle_t handle = (starpu_data_handle_t)inData;
	___starpu_notify_arbitered_dependencies(handle);
}
void _starpu_notify_arbitered_dependencies(starpu_data_handle_t handle)
{
	_starpu_LockOrDelegatePostOrPerform(handle->arbiter, &__starpu_notify_arbitered_dependencies, handle);
}
void ___starpu_notify_arbitered_dependencies(starpu_data_handle_t handle)
{
#else // LOCK_OR_DELEGATE
void _starpu_notify_arbitered_dependencies(starpu_data_handle_t handle)
{
#endif
	starpu_arbiter_t arbiter = handle->arbiter;
#ifndef LOCK_OR_DELEGATE
	STARPU_PTHREAD_MUTEX_LOCK(&arbiter->mutex);
#endif

	/* Since the request has been posted the handle may have been proceed and released */
	if (handle->arbitered_req_list == NULL)
	{
#ifndef LOCK_OR_DELEGATE
		STARPU_PTHREAD_MUTEX_UNLOCK(&arbiter->mutex);
#endif
		return 1;
	}
	/* no one has the right to work on arbitered_req_list without a lock on mutex
	   so we do not need to lock the handle for safety */
	struct _starpu_data_requester *r;
	r = _starpu_data_requester_list_begin(handle->arbitered_req_list); //_head;
	while (r)
	{
		struct _starpu_job* j = r->j;
		unsigned nbuffers = STARPU_TASK_GET_NBUFFERS(j->task);
		unsigned nb_non_arbitered_buff;
		/* find the position of arbiter buffers */
		for (nb_non_arbitered_buff = 0; nb_non_arbitered_buff < nbuffers; nb_non_arbitered_buff++)
		{
			starpu_data_handle_t handle_arbiter = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, nb_non_arbitered_buff);
			if (nb_non_arbitered_buff && (_STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, nb_non_arbitered_buff-1) == handle_arbiter))
				/* We have already requested this data, skip it. This
				 * depends on ordering putting writes before reads, see
				 * _starpu_compar_handles.  */
				continue;
			enum starpu_data_access_mode mode = _STARPU_JOB_GET_ORDERED_BUFFER_MODE(j, nb_non_arbitered_buff);
			if (handle_arbiter->arbiter == arbiter)
			{
				break;
			}
		}

		unsigned idx_buf_arbiter;
		unsigned all_arbiter_available = 1;

		for (idx_buf_arbiter = nb_non_arbitered_buff; idx_buf_arbiter < nbuffers; idx_buf_arbiter++)
		{
			starpu_data_handle_t handle_arbiter = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_arbiter);
			if (idx_buf_arbiter && (_STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_arbiter-1)==handle_arbiter))
				/* We have already requested this data, skip it. This
				 * depends on ordering putting writes before reads, see
				 * _starpu_compar_handles.  */
				continue;
			if (handle_arbiter->arbiter != arbiter)
				/* Will have to process another arbiter, will do that later */
				break;

			/* we post all arbiter  */
			enum starpu_data_access_mode mode = _STARPU_JOB_GET_ORDERED_BUFFER_MODE(j, idx_buf_arbiter);

			_starpu_spin_lock(&handle_arbiter->header_lock);
			if (handle_arbiter->refcnt != 0)
			{
				/* handle is not available, record ourself */
				_starpu_spin_unlock(&handle_arbiter->header_lock);
				all_arbiter_available = 0;
				break;
			}
			/* mark the handle as taken */
			handle_arbiter->refcnt += 1;
			handle_arbiter->current_mode = mode;
			_starpu_spin_unlock(&handle_arbiter->header_lock);
		}

		if (all_arbiter_available)
		{
			for (idx_buf_arbiter = nb_non_arbitered_buff; idx_buf_arbiter < nbuffers; idx_buf_arbiter++)
			{
				starpu_data_handle_t handle_arbiter = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_arbiter);
				if (idx_buf_arbiter && (_STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_arbiter-1)==handle_arbiter))
					continue;
				if (handle_arbiter->arbiter != arbiter)
					break;

				/* we post all arbiter  */
				enum starpu_data_access_mode mode = _STARPU_JOB_GET_ORDERED_BUFFER_MODE(j, idx_buf_arbiter);

				_starpu_spin_lock(&handle_arbiter->header_lock);
				STARPU_ASSERT(handle_arbiter->refcnt == 1);
				STARPU_ASSERT( handle_arbiter->busy_count >= 1);
				STARPU_ASSERT( handle_arbiter->current_mode == mode);
				const unsigned correctly_deleted = remove_job_from_requester_list(handle_arbiter->arbitered_req_list, j);
				STARPU_ASSERT(correctly_deleted == 0);
				if (_starpu_data_requester_list_empty(handle_arbiter->arbitered_req_list)) // If size == 0
				{
					_starpu_data_requester_list_delete(handle_arbiter->arbitered_req_list);
					handle_arbiter->arbitered_req_list = NULL;
				}
				_starpu_spin_unlock(&handle_arbiter->header_lock);
			}
			/* Remove and delete list node */
			_starpu_data_requester_delete(r);

			/* release global mutex */
#ifndef LOCK_OR_DELEGATE
			STARPU_PTHREAD_MUTEX_UNLOCK(&arbiter->mutex);
#endif

			if (idx_buf_arbiter < nbuffers)
			{
				/* Other arbitered data, process them */
				_starpu_submit_job_enforce_arbitered_deps(j, idx_buf_arbiter, nbuffers);
			}
			else
				/* Finished with all data, can eventually push! */
				_starpu_push_task(j);

			/* We need to lock when returning 0 */
			return 0;
		}
		else
		{
			unsigned idx_buf_cancel;
			/* all handles are not available - revert the mark */
			for (idx_buf_cancel = nb_non_arbitered_buff; idx_buf_cancel < idx_buf_arbiter ; idx_buf_cancel++)
			{
				starpu_data_handle_t cancel_handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_cancel);
				if (idx_buf_cancel && (_STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, idx_buf_cancel-1)==cancel_handle))
					continue;
				if (cancel_handle->arbiter != arbiter)
					break;
				_starpu_spin_lock(&cancel_handle->header_lock);
				STARPU_ASSERT(cancel_handle->refcnt == 1);
				cancel_handle->refcnt -= 1;
				_starpu_spin_unlock(&cancel_handle->header_lock);
			}
		}

		r = r->_next;
	}
	/* no task has been pushed */
#ifndef LOCK_OR_DELEGATE
	STARPU_PTHREAD_MUTEX_UNLOCK(&arbiter->mutex);
#endif
	return 1;
}

starpu_arbiter_t starpu_arbiter_create(void)
{
	starpu_arbiter_t res = malloc(sizeof(*res));

#ifdef LOCK_OR_DELEGATE
	res->dlTaskListHead = NULL;
	_starpu_spin_init(&res->dlListLock);
	res->working = 0;
#else /* LOCK_OR_DELEGATE */
	STARPU_PTHREAD_MUTEX_INIT(&res->mutex, NULL);
#endif /* LOCK_OR_DELEGATE */

	return res;
}

void starpu_data_assign_arbiter(starpu_data_handle_t handle, starpu_arbiter_t arbiter)
{
	STARPU_ASSERT_MSG(!handle->arbiter, "handle can only be assigned one arbiter");
	STARPU_ASSERT_MSG(!handle->refcnt, "arbiter can be assigned to handle only right after initialization");
	STARPU_ASSERT_MSG(!handle->busy_count, "arbiter can be assigned to handle only right after initialization");
	handle->arbiter = arbiter;
}
