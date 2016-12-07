/* 
 * Copyright (C) 2016 Lammert Bies
 * Copyright (c) 2013-2016 the Civetweb developers
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */



#include "libhttp-private.h"



/*
 * struct mg_context *mg_start( const struct mg_callbacks *callbacks, void *user_data, const char **options );
 *
 * The function mg_start() functions as the main entry point for the LibHTTP
 * server. The function starts all threads and when finished returns the
 * context to the running server for future reference.
 */

struct mg_context *mg_start( const struct mg_callbacks *callbacks, void *user_data, const char **options ) {

	struct mg_context *ctx;
	const char *name;
	const char *value;
	const char *default_value;
	int idx;
	int ok;
	int workerthreadcount;
	unsigned int i;
	void (*exit_callback)(const struct mg_context *ctx) = NULL;
	struct mg_workerTLS tls;

#if defined(_WIN32)
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
#endif /* _WIN32 */

	/* Allocate context and initialize reasonable general case defaults. */
	if ((ctx = (struct mg_context *)mg_calloc(1, sizeof(*ctx))) == NULL) return NULL;

	/* Random number generator will initialize at the first call */
	ctx->auth_nonce_mask = (uint64_t)get_random() ^ (uint64_t)(ptrdiff_t)(options);

	if (mg_atomic_inc(&sTlsInit) == 1) {

#if defined(_WIN32)
		InitializeCriticalSection(&global_log_file_lock);
#endif /* _WIN32 */
#if !defined(_WIN32)
		pthread_mutexattr_init(&pthread_mutex_attr);
		pthread_mutexattr_settype(&pthread_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
#endif

		if (0 != pthread_key_create(&sTlsKey, tls_dtor)) {
			/* Fatal error - abort start. However, this situation should
			 * never
			 * occur in practice. */
			mg_atomic_dec(&sTlsInit);
			mg_cry(fc(ctx), "Cannot initialize thread local storage");
			mg_free(ctx);
			return NULL;
		}
	} else {
		/* TODO (low): istead of sleeping, check if sTlsKey is already
		 * initialized. */
		mg_sleep(1);
	}

	tls.is_master  = -1;
	tls.thread_idx = (unsigned)mg_atomic_inc(&thread_idx_max);
#if defined(_WIN32)
	tls.pthread_cond_helper_mutex = NULL;
#endif
	pthread_setspecific(sTlsKey, &tls);

#if defined(USE_LUA)
	lua_init_optional_libraries();
#endif

	ok = 0 == pthread_mutex_init(&ctx->thread_mutex, &pthread_mutex_attr);
#if !defined(ALTERNATIVE_QUEUE)
	ok &= 0 == pthread_cond_init(&ctx->sq_empty, NULL);
	ok &= 0 == pthread_cond_init(&ctx->sq_full, NULL);
#endif
	ok &= 0 == pthread_mutex_init(&ctx->nonce_mutex, &pthread_mutex_attr);
	if (!ok) {
		/* Fatal error - abort start. However, this situation should never
		 * occur in practice. */
		mg_cry(fc(ctx), "Cannot initialize thread synchronization objects");
		mg_free(ctx);
		pthread_setspecific(sTlsKey, NULL);
		return NULL;
	}

	if (callbacks) {
		ctx->callbacks = *callbacks;
		exit_callback = callbacks->exit_context;
		ctx->callbacks.exit_context = 0;
	}
	ctx->user_data = user_data;
	ctx->handlers = NULL;

#if defined(USE_LUA) && defined(USE_WEBSOCKET)
	ctx->shared_lua_websockets = 0;
#endif

	while (options && (name = *options++) != NULL) {
		if ((idx = get_option_index(name)) == -1) {
			mg_cry(fc(ctx), "Invalid option: %s", name);
			free_context(ctx);
			pthread_setspecific(sTlsKey, NULL);
			return NULL;
		} else if ((value = *options++) == NULL) {
			mg_cry(fc(ctx), "%s: option value cannot be NULL", name);
			free_context(ctx);
			pthread_setspecific(sTlsKey, NULL);
			return NULL;
		}
		if (ctx->config[idx] != NULL) {
			mg_cry(fc(ctx), "warning: %s: duplicate option", name);
			mg_free(ctx->config[idx]);
		}
		ctx->config[idx] = mg_strdup(value);
		DEBUG_TRACE("[%s] -> [%s]", name, value);
	}

	/* Set default value if needed */
	for (i = 0; config_options[i].name != NULL; i++) {
		default_value = config_options[i].default_value;
		if (ctx->config[i] == NULL && default_value != NULL) {
			ctx->config[i] = mg_strdup(default_value);
		}
	}

#if defined(NO_FILES)
	if (ctx->config[DOCUMENT_ROOT] != NULL) {
		mg_cry(fc(ctx), "%s", "Document root must not be set");
		free_context(ctx);
		pthread_setspecific(sTlsKey, NULL);
		return NULL;
	}
#endif

	get_system_name(&ctx->systemName);

	/* NOTE(lsm): order is important here. SSL certificates must
	 * be initialized before listening ports. UID must be set last. */
	if (!set_gpass_option(ctx) ||
#if !defined(NO_SSL)
	    !set_ssl_option(ctx) ||
#endif
	    !set_ports_option(ctx) ||
#if !defined(_WIN32)
	    !set_uid_option(ctx) ||
#endif
	    !set_acl_option(ctx)) {
		free_context(ctx);
		pthread_setspecific(sTlsKey, NULL);
		return NULL;
	}

#if !defined(_WIN32)
	/* Ignore SIGPIPE signal, so if browser cancels the request, it
	 * won't kill the whole process. */
	(void)signal(SIGPIPE, SIG_IGN);
#endif /* !_WIN32 */

	workerthreadcount = atoi(ctx->config[NUM_THREADS]);

	if (workerthreadcount > MAX_WORKER_THREADS) {
		mg_cry(fc(ctx), "Too many worker threads");
		free_context(ctx);
		pthread_setspecific(sTlsKey, NULL);
		return NULL;
	}

	if (workerthreadcount > 0) {
		ctx->cfg_worker_threads = ((unsigned int)(workerthreadcount));
		ctx->workerthreadids =
		    (pthread_t *)mg_calloc(ctx->cfg_worker_threads, sizeof(pthread_t));
		if (ctx->workerthreadids == NULL) {
			mg_cry(fc(ctx), "Not enough memory for worker thread ID array");
			free_context(ctx);
			pthread_setspecific(sTlsKey, NULL);
			return NULL;
		}

#if defined(ALTERNATIVE_QUEUE)
		ctx->client_wait_events = mg_calloc(sizeof(ctx->client_wait_events[0]),
		                                    ctx->cfg_worker_threads);
		if (ctx->client_wait_events == NULL) {
			mg_cry(fc(ctx), "Not enough memory for worker event array");
			mg_free(ctx->workerthreadids);
			free_context(ctx);
			pthread_setspecific(sTlsKey, NULL);
			return NULL;
		}

		ctx->client_socks =
		    mg_calloc(sizeof(ctx->client_socks[0]), ctx->cfg_worker_threads);
		if (ctx->client_wait_events == NULL) {
			mg_cry(fc(ctx), "Not enough memory for worker socket array");
			mg_free(ctx->client_socks);
			mg_free(ctx->workerthreadids);
			free_context(ctx);
			pthread_setspecific(sTlsKey, NULL);
			return NULL;
		}

		for (i = 0; (unsigned)i < ctx->cfg_worker_threads; i++) {
			ctx->client_wait_events[i] = event_create();
			if (ctx->client_wait_events[i] == 0) {
				mg_cry(fc(ctx), "Error creating worker event %i", i);
				/* TODO: clean all and exit */
			}
		}
#endif
	}

#if defined(USE_TIMERS)
	if (timers_init(ctx) != 0) {
		mg_cry(fc(ctx), "Error creating timers");
		free_context(ctx);
		pthread_setspecific(sTlsKey, NULL);
		return NULL;
	}
#endif

	/* Context has been created - init user libraries */
	if (ctx->callbacks.init_context) {
		ctx->callbacks.init_context(ctx);
	}
	ctx->callbacks.exit_context = exit_callback;
	ctx->context_type = 1; /* server context */

	/* Start master (listening) thread */
	mg_start_thread_with_id(master_thread, ctx, &ctx->masterthreadid);

	/* Start worker threads */
	for (i = 0; i < ctx->cfg_worker_threads; i++) {
		struct worker_thread_args *wta =
		    mg_malloc(sizeof(struct worker_thread_args));
		if (wta) {
			wta->ctx = ctx;
			wta->index = (int)i;
		}

		if ((wta == NULL)
		    || (mg_start_thread_with_id(worker_thread,
		                                wta,
		                                &ctx->workerthreadids[i]) != 0)) {

			/* thread was not created */
			if (wta != NULL) {
				mg_free(wta);
			}

			if (i > 0) {
				mg_cry(fc(ctx),
				       "Cannot start worker thread %i: error %ld",
				       i + 1,
				       (long)ERRNO);
			} else {
				mg_cry(fc(ctx),
				       "Cannot create threads: error %ld",
				       (long)ERRNO);
				free_context(ctx);
				pthread_setspecific(sTlsKey, NULL);
				return NULL;
			}
			break;
		}
	}

	pthread_setspecific(sTlsKey, NULL);
	return ctx;

}  /* mg_start */