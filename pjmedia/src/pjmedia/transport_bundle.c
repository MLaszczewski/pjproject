/* $Id$ */
/*
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2018 Viamage LTD (http://viamage.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <pjmedia/transport_bundle.h>
#include <pjmedia/endpoint.h>
#include <pjlib-util/base64.h>
#include <pj/array.h>
#include <pj/assert.h>
#include <pj/ctype.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/pool.h>

#define MAX_BUNDLE_STREAMS 16

/* bundle transport endpoint */
typedef struct transport_bundle_endpoint
{
    pjmedia_transport	 base;		    /**< Base transport interface.  */
    
    pj_uint32_t ssrc;
    pj_uint32_t rem_ssrc;    

    /* Stream information */
    void		*user_data;
    void		(*rtp_cb)( void *user_data,
				   void *pkt,
				   pj_ssize_t size);
    void		(*rtcp_cb)(void *user_data,
				   void *pkt,
				   pj_ssize_t size);

    /* Transport information */
    transport_bundle	*bundle; /**< Underlying transport.       */
    
} transport_bundle_endpoint;

/* bundle transport */
typedef struct transport_bundle
{
    pjmedia_transport	 base;		    /**< Base transport interface.  */
    
    pj_pool_t		*pool;		    /**< Pool for transport bundle.   */
    pj_lock_t		*mutex;		    /**< Mutex */

    /* Transport information */
    pjmedia_transport	*member_tp; /**< Underlying transport.       */
    pj_bool_t		 member_tp_attached;
    
    /* Endpoints */
    transport_bundle_endpoint* endpoints[MAX_BUNDLE_STREAMS];
        
} transport_bundle;

/*
 * This callback is called by transport when incoming rtp is received
 */
static void bundle_rtp_cb( void *user_data, void *pkt, pj_ssize_t size);

/*
 * This callback is called by transport when incoming rtcp is received
 */
static void bundle_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size);


static pj_status_t transport_get_info (pjmedia_transport *tp,
				       pjmedia_transport_info *info);
				       
static void	   transport_detach   (pjmedia_transport *tp,
				       void *strm);
static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
				       const pj_sockaddr_t *addr,
				       unsigned addr_len,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_media_create(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       unsigned options,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_media_start (pjmedia_transport *tp,
				       pj_pool_t *pool,
				       const pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_media_stop(pjmedia_transport *tp);
static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
				       pjmedia_dir dir,
				       unsigned pct_lost);
static pj_status_t transport_destroy  (pjmedia_transport *tp);
static pj_status_t transport_attach2  (pjmedia_transport *tp,
				       pjmedia_transport_attach_param *param);
				       
static pjmedia_transport_op transport_bundle_op =
{
    &transport_get_info,
    NULL, //&transport_attach,
    NULL, //&transport_detach,
    &transport_send_rtp,
    &transport_send_rtcp,
    &transport_send_rtcp2,
    &transport_media_create,
    &transport_encode_sdp,
    &transport_media_start,
    &transport_media_stop,
    &transport_simulate_lost,
    &transport_destroy,
    NULL, //&transport_attach2
};


static pj_status_t transport_endpoint_get_info (pjmedia_transport *tp,
				       pjmedia_transport_info *info);
				       
static void	   transport_endpoint_detach   (pjmedia_transport *tp,
				       void *strm);
static pj_status_t transport_endpoint_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_endpoint_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_endpoint_send_rtcp2(pjmedia_transport *tp,
				       const pj_sockaddr_t *addr,
				       unsigned addr_len,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_endpoint_media_create(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       unsigned options,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_endpoint_encode_sdp(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_endpoint_media_start (pjmedia_transport *tp,
				       pj_pool_t *pool,
				       const pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_endpoint_media_stop(pjmedia_transport *tp);
static pj_status_t transport_endpoint_simulate_lost(pjmedia_transport *tp,
				       pjmedia_dir dir,
				       unsigned pct_lost);
static pj_status_t transport_endpoint_destroy  (pjmedia_transport *tp);
static pj_status_t transport_endpoint_attach2  (pjmedia_transport *tp,
				       pjmedia_transport_attach_param *param);p
				       
static pjmedia_transport_op transport_bundle_endpoint_op =
{
    &transport_endpoint_get_info,
    NULL, //&transport_attach,
    &transport_endpoint_detach,
    &transport_endpoint_send_rtp,
    &transport_endpoint_send_rtcp,
    &transport_endpoint_send_rtcp2,
    &transport_endpoint_media_create,
    &transport_endpoint_encode_sdp,
    &transport_endpoint_media_start,
    &transport_endpoint_media_stop,
    &transport_endpoint_simulate_lost,
    &transport_endpoint_destroy,
    &transport_endpoint_attach2
};


static const pj_str_t ID_BUNDLE = { "bundle", 6 };
static const pj_str_t ID_BUNDLE_ENDPOINT = { "bundle-endpoint", 15 };

/*
 * Create an BUNDLE media transport.
 */
PJ_DEF(pj_status_t) pjmedia_transport_bundle_create(
				       pjmedia_endpt *endpt,
				       pjmedia_transport *tp,
				       pjmedia_transport **p_tp)
{
    pj_pool_t *pool;
    transport_bundle *bundle;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(endpt && tp && p_tp, PJ_EINVAL);

    pool = pjmedia_endpt_create_pool(endpt, "bundle%p", 1000, 1000);
    bundle = PJ_POOL_ZALLOC_T(pool, transport_bundle);

    bundle->pool = pool;
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
		bundle->endpoints[i] = NULL;
	}

    status = pj_lock_create_recursive_mutex(pool, pool->obj_name, &mux->mutex);
    if (status != PJ_SUCCESS) {
	  pj_pool_release(pool);
	  return status;
    }

    /* Initialize base pjmedia_transport */
    pj_memcpy(bundle->base.name, pool->obj_name, PJ_MAX_OBJ_NAME);
    if (tp)
	  bundle->base.type = tp->type;
    else
	  bundle->base.type = PJMEDIA_TRANSPORT_TYPE_UDP;
    bundle->base.op = &transport_bundle_op;

    /* Set underlying transport */
    bundle->member_tp = tp;

    /* Done */
    *p_tp = &bundle->base;

    return PJ_SUCCESS;
}


static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    pj_status_t status;
    transport_bundle *bundle = (transport_bundle*) tp;
    return pjmedia_transport_send_rtp(bundle->member_tp, pkt, size);
}

static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    return transport_send_rtcp2(tp, NULL, 0, pkt, size);
}

static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
				        const pj_sockaddr_t *addr,
				        unsigned addr_len,
				        const void *pkt,
				        pj_size_t size)
{
   pj_status_t status;
   transport_bundle *bundle = (transport_bundle*) tp;
   return pjmedia_transport_send_rtcp(bundle->member_tp, pkt, size);
}

static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
					   pjmedia_dir dir,
					   unsigned pct_lost)
{
    transport_bundle *bundle = (transport_bundle *) tp;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    return pjmedia_transport_simulate_lost(bundle->member_tp, dir, pct_lost);
}

static pj_status_t transport_destroy  (pjmedia_transport *tp)
{
    transport_bundle *mux = (transport_bundle *) tp;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
	    bundle->endpoints[i]->bundle = NULL;
	}

    /* In case mutex is being acquired by other thread */
    pj_lock_acquire(mux->mutex);
    pj_lock_release(mux->mutex);

    pj_lock_destroy(mux->mutex);
    pj_pool_release(mux->pool);

    return PJ_SUCCESS;
}

/*
 * This callback is called by transport when incoming rtp is received
 */
static void bundle_rtp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_mux *bundle = (transport_mux *) user_data;
    int i;
    
    pjmedia_rtp_hdr* hdr = (pjmedia_rtp_hdr*)pkt;   
    pj_uint32_t ssrc = hdr.ssrc;
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
		transport_bundle_endpoint* stream = bundle->endpoints[i];
		if(!stream) continue;
		if((stream->ssrc == ssrc || stream->rem_ssrc == ssrc) && stream->rtp_cb) stream->rtp_cb(stream->user_data, pkt, size);
	}
  
    return;
}

/*
 * This callback is called by transport when incoming rtcp is received
 */
static void bundle_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_mux *bundle = (transport_mux *) user_data;   
    
    
    
    /// TODO parse RTCP packet and demux reports
    /// Send to endpoints matching ssrc   
    
    return;
}


PJ_DECL(pj_status_t) pjmedia_transport_bundle_endpoint_create(
				       pjmedia_transport *tp,
				       pj_uint32_t ssrc,
				       pj_uint32_t rem_ssrc,
				       pjmedia_transport **p_tpe) {
    pj_pool_t *pool;
    transport_bundle_endpoint *endpoint;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp && p_tpe, PJ_EINVAL);
    
    transport_bundle* bundle = (transport_bundle*)tp;
    
    pool = bundle.pool;
    
    endpoint = PJ_POOL_ZALLOC_T(pool, transport_bundle_endpoint);

    endpoint->bundle = bundle;
    
    pj_lock_acquire(bundle->mutex);
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
		if(bundle->endpoints[i] == NULL) {
			bundle->endpoints[i] = endpoint;
		}
	}

    /* Initialize base pjmedia_transport */
    pj_memcpy(endpoint->base.name, pool->obj_name, PJ_MAX_OBJ_NAME);
    if (tp)
	  endpoint->base.type = tp->type;
    else
	  endpoint->base.type = PJMEDIA_TRANSPORT_TYPE_UDP;
    bundle->base.op = &transport_bundle_op;

    /* Done */
    *p_tpe = &bundle->base;

    return PJ_SUCCESS;
}



