/*
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <compiler.h>
#include <stdbool.h>
#include <string.h>
#include <sys/queue.h>
#include <tee_api.h>
#include <tee_ta_api.h>
#include <tee_internal_api_extensions.h>
#include <user_ta_header.h>
#include <utee_syscalls.h>
#include "utee_misc.h"
#include <tee_arith_internal.h>
#include <malloc.h>
#include "tee_api_private.h"
// SNOWFLY
#include <printk.h>

/*
 * Pull in symbol __utee_mcount.
 * This symbol is implemented in assembly in its own compilation unit, and is
 * never referenced except by the linker script (in a PROVIDE() command).
 * Because the compilation units are packed into an archive (libutee.a), the
 * linker will discard the compilation units that are not explicitly
 * referenced. AFAICT this occurs *before* the linker processes the PROVIDE()
 * command, resulting in an "undefined symbol" error. We avoid this by
 * adding an explicit reference here.
 */
extern uint8_t __utee_mcount[];
void *_ref__utee_mcount __unused = &__utee_mcount;

struct ta_session {
	uint32_t session_id;
	void *session_ctx;
	TAILQ_ENTRY(ta_session) link;
};

// SNOWFLY
/*
struct mproc {
	uint32_t mp_num;	
	int mp_endpoint;
	int mp_father;
};
*/

static TAILQ_HEAD(ta_sessions, ta_session) ta_sessions =
		TAILQ_HEAD_INITIALIZER(ta_sessions);

static uint32_t ta_ref_count;
static bool context_init;

/* From user_ta_header.c, built within TA */
extern uint8_t ta_heap[];
extern const size_t ta_heap_size;

uint32_t ta_param_types;
TEE_Param ta_params[TEE_NUM_PARAMS];

static void ta_header_save_params(uint32_t param_types,
				  TEE_Param params[TEE_NUM_PARAMS])
{
	ta_param_types = param_types;

	if (params)
		memcpy(ta_params, params, sizeof(ta_params));
	else
		memset(ta_params, 0, sizeof(ta_params));
}

static struct ta_session *ta_header_get_session(uint32_t session_id)
{
	struct ta_session *itr;

	TAILQ_FOREACH(itr, &ta_sessions, link) {
		if (itr->session_id == session_id)
			return itr;
	}
	return NULL;
}

static TEE_Result ta_header_add_session(uint32_t session_id)
{
	struct ta_session *itr = ta_header_get_session(session_id);

	if (itr)
		return TEE_SUCCESS;

	ta_ref_count++;

	if (ta_ref_count == 1) {
		TEE_Result res;

		if (!context_init) {
			trace_set_level(tahead_get_trace_level());
			__utee_gprof_init();
			malloc_add_pool(ta_heap, ta_heap_size);
			_TEE_MathAPI_Init();
			context_init = true;
		}

		res = TA_CreateEntryPoint();
		if (res != TEE_SUCCESS)
			return res;
	}

	itr = TEE_Malloc(sizeof(struct ta_session),
			TEE_USER_MEM_HINT_NO_FILL_ZERO);
	if (!itr)
		return TEE_ERROR_OUT_OF_MEMORY;
	itr->session_id = session_id;
	itr->session_ctx = 0;
	TAILQ_INSERT_TAIL(&ta_sessions, itr, link);

	return TEE_SUCCESS;
}

static void ta_header_remove_session(uint32_t session_id)
{
	struct ta_session *itr;

	TAILQ_FOREACH(itr, &ta_sessions, link) {
		if (itr->session_id == session_id) {
			TAILQ_REMOVE(&ta_sessions, itr, link);
			TEE_Free(itr);

			ta_ref_count--;
			if (ta_ref_count == 0) {
				__utee_gprof_fini();
				TA_DestroyEntryPoint();
			}

			return;
		}
	}
}

static TEE_Result entry_open_session(unsigned long session_id,
			struct utee_params *up)
{
	TEE_Result res;
	struct ta_session *session;
	uint32_t param_types;
	TEE_Param params[TEE_NUM_PARAMS];

	res = ta_header_add_session(session_id);
	if (res != TEE_SUCCESS)
		return res;

	session = ta_header_get_session(session_id);
	if (!session)
		return TEE_ERROR_BAD_STATE;

	__utee_to_param(params, &param_types, up);
	ta_header_save_params(param_types, params);

	res = TA_OpenSessionEntryPoint(param_types, params,
				       &session->session_ctx);

	__utee_from_param(up, param_types, params);

	if (res != TEE_SUCCESS)
		ta_header_remove_session(session_id);
	return res;
}

static TEE_Result entry_close_session(unsigned long session_id)
{
	struct ta_session *session = ta_header_get_session(session_id);

	if (!session)
		return TEE_ERROR_BAD_STATE;

	TA_CloseSessionEntryPoint(session->session_ctx);

	ta_header_remove_session(session_id);
	return TEE_SUCCESS;
}

static TEE_Result entry_invoke_command(unsigned long session_id,
			struct utee_params *up, unsigned long cmd_id)
{
	TEE_Result res;
	uint32_t param_types;
	TEE_Param params[TEE_NUM_PARAMS];
	struct ta_session *session = ta_header_get_session(session_id);

	if (!session)
		return TEE_ERROR_BAD_STATE;

	__utee_to_param(params, &param_types, up);
	ta_header_save_params(param_types, params);

	res = TA_InvokeCommandEntryPoint(session->session_ctx, cmd_id,
					 param_types, params);

	__utee_from_param(up, param_types, params);
	return res;
}

void __noreturn __utee_entry(unsigned long func, unsigned long session_id,
			struct utee_params *up, unsigned long cmd_id)
{
	TEE_Result res;

	switch (func) {
	case UTEE_ENTRY_FUNC_OPEN_SESSION:
		res = entry_open_session(session_id, up);
		break;
	case UTEE_ENTRY_FUNC_CLOSE_SESSION:
		res = entry_close_session(session_id);
		break;
	case UTEE_ENTRY_FUNC_INVOKE_COMMAND:
		res = entry_invoke_command(session_id, up, cmd_id);
		break;
	default:
		res = 0xffffffff;
		TEE_Panic(0);
		break;
	}
	ta_header_save_params(0, NULL);
	utee_return(res);
}
// SNOWFLY

/*
void __noreturn __sn_utee_entry(void)
{
	unsigned num = 0;
	int temp = 0;
	struct message m;

	m.u.msg[0] = 'a';
	m.u.msg[1] = 0;
	while(1) {
		if(num % (64*1024*1024) == 0 && temp<5) {
			m.u.msg[0] += 1;
			printf("process A:I sendrec a msg to 1:%s\n", m.u.msg);
			if(sn_sendrec(1, &m) != 0)
				printf("send error\n");
			else
				printf("process A sendrec suc:%s\n", m.u.msg);
			temp++;
		}
		num += 1;
	}
}
*/

/*
void __noreturn __sn_utee_entry(void)
{
	unsigned num = 0;
	struct message m;

	if(sn_receive(0, &m) != 0)
		printf("recv error\n");
	else
		printf("process B:I got a msg from 0:%s\n", m.u.msg);
	while(1) {
		if(num % (64*1024*1024) == 0) {
			m.u.msg[0] += 1;
			printf("process B:I sendrec a msg to 0: %s\n", m.u.msg);
			if(sn_sendrec(0, &m) != 0)
				printf("process B sendrec error!\n");
			else
				printf("process B sendrec suc: %s\n", m.u.msg);
		}
		num += 1;
	}
}
*/
/*
void __noreturn __sn_utee_entry(void)
{
	unsigned num = 0;
	int temp = 0;
	int res = fork();
	if(res<0)
		printf("fork error!\n");
	else if(res == 0)
		printf("I am father\n");
	else
		printf("I am child\n");
	while(1) {
		if(num % (64*1024*1024) == 0 && temp<5) {
			printf("hello res %d\n", res);
			temp++;
		}
		num += 1;
	}
}
*/

//*
//struct mproc mprocs[16];
void __sn_utee_entry(void)
{
/*
	struct message msg;
	int res;

	trace_ext_puts("==================================================================================\n");
	trace_ext_puts("        This is PM, I am waiting for message\n");
	mprocs[0].mp_endpoint = 0;
	mprocs[1].mp_endpoint = 1;
	while(1) {
		res = sn_receive(-1, &msg);
		if(res != 0) {
			trace_ext_puts("PM receive error!\n");
			continue;
		}
		if(msg.type == M_TYPE_FORK) {
			printf("        PM:I got a FORK msg form %d\n", msg.from);
			res = pm_fork(msg.from);
			if(res < 0)
				msg.u.mp_pid = -1;
			else
				msg.u.mp_pid = 0;
			sn_send(msg.from, &msg);
			if(res >= 0) {
				msg.u.mp_pid = res;
				sn_send(res, &msg);
			}
		}
	}
*/
	ta_main();
}
//*/

/*
void __noreturn __sn_utee_entry(void)
{
	unsigned num = 0;
	int temp = 0;
	int res = fork();
	char parent = 'a';
	char child = '0';
	if(res<0)
		trace_ext_puts("fork error!\n");
	else if(res == 0)
		trace_ext_puts("This is process 1:I am father\n");
	else
		printf("This is process %d:I am child\n", res);
	if(res == 0) { // parent process
		while(1) {
			if(num % (32*1024*1024) == 0 && temp<14) {
				printf("process 1: %c\n", parent);
				parent++;
				if(parent == 'i')
					sleep(4);
				if(parent > 'z')
					parent = 'a';
				temp++;
			}
			num += 1;
		}
	}else{
		while(1) {
			if(num % (32*1024*1024) == 0 && temp<22) {
				printf("process %d: %c\n", res, child);
				child++;
				if(child > '9')
					child = '0';
				temp++;
			}
			num += 1;
		}
	}
}
*/
