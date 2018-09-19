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
#include <pj/sock.h>

#if 0
#   define TRACE_(x)	PJ_LOG(3,x)
#else
#   define TRACE_(x)	;
#endif

#define MAX_BUNDLE_STREAMS 16
#define RTCP_BUFFER_SIZE 1024

typedef struct transport_bundle transport_bundle;

/* bundle transport endpoint */
typedef struct transport_bundle_endpoint
{
    pjmedia_transport	 base;		    /**< Base transport interface.  */

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
    
    pj_uint32_t ssrc;
    pj_uint32_t rem_ssrc;    
    
    char rtcp_buffer[RTCP_BUFFER_SIZE];
    int rtcp_buffer_position;
    
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
        
    /* Protection from multiple calls of member functions */
    pj_bool_t  media_created;
    pj_bool_t  media_encoded;
    pj_bool_t  media_started;    
        
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
				       pjmedia_transport_attach_param *param);
				       
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

    status = pj_lock_create_recursive_mutex(pool, pool->obj_name, &bundle->mutex);
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
    
    bundle->member_tp_attached = PJ_FALSE; 

    /* Done */
    *p_tp = &bundle->base;

    return PJ_SUCCESS;
}


static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    transport_bundle *bundle = (transport_bundle*) tp;  
    PJ_ASSERT_RETURN(tp, PJ_EINVAL);  
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
   transport_bundle *bundle = (transport_bundle*) tp;
   PJ_ASSERT_RETURN(tp, PJ_EINVAL);
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
    transport_bundle *bundle = (transport_bundle *) tp;
   // pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
	    bundle->endpoints[i]->bundle = NULL;
	}

    /* In case mutex is being acquired by other thread */
    pj_lock_acquire(bundle->mutex);
    pj_lock_release(bundle->mutex);

    pj_lock_destroy(bundle->mutex);
    pj_pool_release(bundle->pool);

    return PJ_SUCCESS;
}

/*
 * This callback is called by transport when incoming rtp is received
 */
static void bundle_rtp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_bundle *bundle = (transport_bundle *) user_data;
    int i;
    
    pjmedia_rtp_hdr* hdr = (pjmedia_rtp_hdr*)pkt;   
    pj_uint32_t ssrc = pj_htonl(hdr->ssrc);
    
    //fprintf(stderr,"Received RTP with size %d and SSRC: %x -> %x\n",  (int)size, hdr->ssrc, ssrc);
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
		transport_bundle_endpoint* stream = bundle->endpoints[i];
		if(!stream) continue;
		//fprintf(stderr,"Candidate stream SSRCs: %x <-> %x\n",  stream->ssrc, stream->rem_ssrc);
		if((stream->ssrc == ssrc || stream->rem_ssrc == ssrc) && stream->rtp_cb) stream->rtp_cb(stream->user_data, pkt, size);
	}
  
    return;
}

#define RTCP_SR   200
#define RTCP_RR   201
#define RTCP_SDES 202
#define RTCP_BYE  203
#define RTCP_XR   207
/* RTCP Feedbacks */
#define RTCP_RTPFB	205
#define RTCP_PSFB	206

static void parse_rtcp_report(transport_bundle *bundle,
			       const void *pkt,
			       pj_size_t size) {
					   
	pjmedia_rtcp_common *common = (pjmedia_rtcp_common*) pkt;
    pjmedia_rtcp_rr *rr = NULL;
    //pjmedia_rtcp_sr *sr = NULL;
    int i;

    const void* p = pkt + sizeof(pjmedia_rtcp_common);
    pj_size_t more = size - sizeof(pjmedia_rtcp_common);
    
    pjmedia_rtcp_common *stream_commons[MAX_BUNDLE_STREAMS];
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
		stream_commons[i] = NULL;
	}
	
	uint32_t common_ssrc = pj_htonl(common->ssrc);
	
	if( common->pt == RTCP_SR) fprintf(stderr, "  type: SR\n");
	if( common->pt == RTCP_RR) fprintf(stderr, "  type: RR\n");
    
    if (common->pt == RTCP_SR) {		
		//sr = (pjmedia_rtcp_sr*) p;		
		p += sizeof(pjmedia_rtcp_sr);
		more -= sizeof(pjmedia_rtcp_sr);
		
	    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
			transport_bundle_endpoint* stream = bundle->endpoints[i];
			if(!stream) continue;
			if(stream->ssrc == common_ssrc || stream->rem_ssrc == common_ssrc) {
				if(!stream->rtcp_cb) continue;
				stream_commons[i] = (pjmedia_rtcp_common*)(stream->rtcp_buffer + stream->rtcp_buffer_position);
				if(stream->rtcp_buffer_position + sizeof(pjmedia_rtcp_common) + sizeof(pjmedia_rtcp_sr) > RTCP_BUFFER_SIZE) continue;
				memcpy(stream_commons[i], common, sizeof(pjmedia_rtcp_common) + sizeof(pjmedia_rtcp_sr));
				stream->rtcp_buffer_position += sizeof(pjmedia_rtcp_common) + sizeof(pjmedia_rtcp_sr);
			} 
		}	
    } 
    
    if (common->pt == RTCP_RR || common->pt == RTCP_SR) {
		for(int i = 0 ; i < common->count; i++) {
			if(more < sizeof(pjmedia_rtcp_rr)) break;
			rr = (pjmedia_rtcp_rr*) p;
			p += sizeof(pjmedia_rtcp_rr);
			more -= sizeof(pjmedia_rtcp_rr);	
			
			uint32_t ssrc = pj_htonl(rr->ssrc);
			
			for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
				transport_bundle_endpoint* stream = bundle->endpoints[i];
				if(!stream) continue;
				if(stream->ssrc == ssrc || stream->rem_ssrc == ssrc) {
					if(!stream->rtcp_cb) continue;
					if(!stream_commons[i]) {
						if(stream->rtcp_buffer_position + sizeof(pjmedia_rtcp_common) + sizeof(pjmedia_rtcp_rr) > RTCP_BUFFER_SIZE) continue;
						stream_commons[i] = (pjmedia_rtcp_common*)(stream->rtcp_buffer + stream->rtcp_buffer_position);
						memcpy(stream_commons[i], common, sizeof(pjmedia_rtcp_common));						
					}
					if(stream->rtcp_buffer_position + sizeof(pjmedia_rtcp_rr) > RTCP_BUFFER_SIZE) continue;										
					memcpy(stream->rtcp_buffer + stream->rtcp_buffer_position, rr, sizeof(pjmedia_rtcp_rr));
					stream->rtcp_buffer_position += sizeof(pjmedia_rtcp_rr);
				} 
			}	
		}		
	} 		   
}

static void parse_rtcp_sdes( transport_bundle *bundle,
			       const void *pkt,
			       pj_size_t size) {
	int i;
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) if(bundle->endpoints[i]) {
	    transport_bundle_endpoint* stream = bundle->endpoints[i];
		if(stream->rtcp_buffer_position + size > RTCP_BUFFER_SIZE) continue;
		memcpy(stream->rtcp_buffer + stream->rtcp_buffer_position, pkt, size);
		stream->rtcp_buffer_position += size;
    }
}

static void parse_rtcp_bye( transport_bundle *bundle,
			       const void *pkt,
			       pj_size_t size) {
	int i;
	for(i = 0; i < MAX_BUNDLE_STREAMS; i++) if(bundle->endpoints[i]) {
	    transport_bundle_endpoint* stream = bundle->endpoints[i];
		if(stream->rtcp_buffer_position + size > RTCP_BUFFER_SIZE) continue;
		memcpy(stream->rtcp_buffer + stream->rtcp_buffer_position, pkt, size);
		stream->rtcp_buffer_position += size;
    }				   
}

static void parse_rtcp_fb( transport_bundle *bundle,
			       const void *pkt,
			       pj_size_t size) {
	int i;
	for(i = 0; i < MAX_BUNDLE_STREAMS; i++) if(bundle->endpoints[i]) {
	    transport_bundle_endpoint* stream = bundle->endpoints[i];
		if(stream->rtcp_buffer_position + size > RTCP_BUFFER_SIZE) continue;
		memcpy(stream->rtcp_buffer + stream->rtcp_buffer_position, pkt, size);
		stream->rtcp_buffer_position += size;
    }				   
}


/*
 * This callback is called by transport when incoming rtcp is received
 */
static void bundle_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_bundle *bundle = (transport_bundle *) user_data;   
    int i;
    
    fprintf(stderr,"Received RTCP with size: %d\n", (int)size);
    
    pj_lock_acquire(bundle->mutex);
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) if(bundle->endpoints[i]) {	
		transport_bundle_endpoint* stream = bundle->endpoints[i];
		stream->rtcp_buffer_position = 0;
	}
    
    pj_uint8_t *p, *p_end;   

    p = (pj_uint8_t*)pkt;
    p_end = p + size;
    while (p < p_end) {
		pjmedia_rtcp_common *common = (pjmedia_rtcp_common*)p;
		uint32_t ssrc = pj_htonl(common->ssrc);
		//fprintf(stderr,"  SSRC: %x\n",  ssrc);
		unsigned len;		      

		len = (pj_ntohs((pj_uint16_t)common->length)+1) * 4;
		switch(common->pt) {
		case RTCP_SR:
		case RTCP_RR:
		case RTCP_XR:
			parse_rtcp_report(bundle, p, len);
			break;
		case RTCP_SDES:
			parse_rtcp_sdes(bundle, p, len);
			break;
		case RTCP_BYE:
			parse_rtcp_bye(bundle, p, len);
			break;
		case RTCP_RTPFB:
		case RTCP_PSFB:
			parse_rtcp_fb(bundle, p, len);
			break;
		default:
			/* Ignore unknown RTCP */
			TRACE_((sess->name, "Received unknown RTCP packet type=%d",
				common->pt));
			break;
		}

		p += len;
    }
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) if(bundle->endpoints[i]) {
		transport_bundle_endpoint* stream = bundle->endpoints[i];
		if(stream->rtcp_buffer_position > 0) {
			fprintf(stderr,"  RTCP DELIVERED TO: %x <-> %x\n",  bundle->endpoints[i]->ssrc, bundle->endpoints[i]->rem_ssrc);
			stream->rtcp_cb(stream->user_data, stream->rtcp_buffer, stream->rtcp_buffer_position);
		}
    }
    
    pj_lock_release(bundle->mutex);
    
    return;
}


PJ_DECL(pj_status_t) pjmedia_transport_bundle_endpoint_create(
				       pjmedia_transport *tp,
				       pj_uint32_t ssrc,
				       pj_uint32_t rem_ssrc,
				       pjmedia_transport **p_tpe) {
    pj_pool_t *pool;
    transport_bundle_endpoint *endpoint;
    unsigned i;

    PJ_ASSERT_RETURN(tp && p_tpe, PJ_EINVAL);
    
    transport_bundle* bundle = (transport_bundle*)tp;
    
    pool = bundle->pool;
    
    endpoint = PJ_POOL_ZALLOC_T(pool, transport_bundle_endpoint);

    endpoint->bundle = bundle;
    
    endpoint->rtcp_buffer_position = 0;
    
    endpoint->ssrc = ssrc;
    endpoint->rem_ssrc = rem_ssrc;
    fprintf(stdout, "CREATE ENDPOINT FOR SSRCs %x <-> %x\n", ssrc, rem_ssrc);
        
    pj_lock_acquire(bundle->mutex);
    
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
		if(bundle->endpoints[i] == NULL) {
			bundle->endpoints[i] = endpoint;
			break;
		}
	}
	
	PJ_ASSERT_RETURN( i < MAX_BUNDLE_STREAMS, PJ_EINVAL);

    /* Initialize base pjmedia_transport */
    pj_memcpy(endpoint->base.name, ID_BUNDLE_ENDPOINT.ptr, ID_BUNDLE_ENDPOINT.slen + 1);
    if (tp)
	  endpoint->base.type = tp->type;
    else
	  endpoint->base.type = PJMEDIA_TRANSPORT_TYPE_UDP;
    endpoint->base.op = &transport_bundle_endpoint_op;

    /* Done */
    *p_tpe = &endpoint->base;
    
    fprintf(stdout, "CREATED ENDPOINT FOR SSRCs %x <-> %x\n", (uint32_t)endpoint, endpoint->ssrc, endpoint->rem_ssrc);
    
    pj_lock_release(bundle->mutex);

    return PJ_SUCCESS;
}

static pj_status_t transport_get_info(pjmedia_transport *tp,
				      pjmedia_transport_info *info)
{
    transport_bundle *bundle = (transport_bundle*) tp;
    return pjmedia_transport_get_info(bundle->member_tp, info);
}

static void transport_detach(pjmedia_transport *tp, void *strm)
{
    transport_bundle *bundle = (transport_bundle*) tp;

    PJ_UNUSED_ARG(strm);
    PJ_ASSERT_ON_FAIL(tp, return);

    if (bundle->member_tp) {
	  pjmedia_transport_detach(bundle->member_tp, bundle);
	  bundle->member_tp_attached = PJ_FALSE;
	  bundle->member_tp = NULL;
    }
}

static pj_status_t transport_media_create(pjmedia_transport *tp,
				          pj_pool_t *sdp_pool,
					  unsigned options,
				          const pjmedia_sdp_session *sdp_remote,
					  unsigned media_index)
{	
    struct transport_bundle *bundle = (struct transport_bundle*) tp;
    pj_status_t status;
    status = pjmedia_transport_media_create(bundle->member_tp, sdp_pool,
					    options, sdp_remote,
					    media_index);		
	bundle->media_created = PJ_TRUE;
	return status;
}


static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
					pj_pool_t *sdp_pool,
					pjmedia_sdp_session *sdp_local,
					const pjmedia_sdp_session *sdp_remote,
					unsigned media_index)
{
    
    struct transport_bundle *bundle = (struct transport_bundle*) tp;
    pj_status_t status;

    PJ_ASSERT_RETURN(tp && sdp_pool && sdp_local, PJ_EINVAL);
       
    status = pjmedia_transport_encode_sdp(bundle->member_tp, sdp_pool,
				  sdp_local, sdp_remote, media_index);

	bundle->media_encoded = PJ_TRUE;
	
    return status;
}


static pj_status_t transport_media_start(pjmedia_transport *tp,
				         pj_pool_t *pool,
				         const pjmedia_sdp_session *sdp_local,
				         const pjmedia_sdp_session *sdp_remote,
				         unsigned media_index)
{
    struct transport_bundle *bundle = (struct transport_bundle*) tp;
    pj_status_t status;

    PJ_ASSERT_RETURN(tp && pool && sdp_local && sdp_remote, PJ_EINVAL);

    status = pjmedia_transport_media_start(bundle->member_tp, pool,
					   sdp_local, sdp_remote,
				           media_index);    
				           
    bundle->media_started = PJ_TRUE;				           
    return status;
}

static pj_status_t transport_media_stop(pjmedia_transport *tp)
{
    struct transport_bundle *bundle = (struct transport_bundle*) tp;
    pj_status_t status;


    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    /* Invoke media_stop() of member tp */
    status = pjmedia_transport_media_stop(bundle->member_tp);
    if (status != PJ_SUCCESS)
	PJ_LOG(4, (bundle->pool->obj_name,
		   "RTCP-bundle failed stop underlying media transport."));
		   
    bundle->media_started = PJ_FALSE;
    		   
    return status;
}

static pj_status_t transport_attach2(pjmedia_transport *tp,
				     pjmedia_transport_attach_param *param)
{
    transport_bundle *bundle = (transport_bundle*) tp;
    pjmedia_transport_attach_param member_param;
    
    pj_status_t status;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    if(bundle->member_tp_attached) return PJ_SUCCESS;
    
    fprintf(stderr, "BUNDLE ATTACH2!\n");
    
    /* Attach self to member transport */
    member_param = *param;
    member_param.user_data = bundle;
    member_param.rtp_cb = &bundle_rtp_cb;
    member_param.rtcp_cb = &bundle_rtcp_cb;
    status = pjmedia_transport_attach2(bundle->member_tp, &member_param);

    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    
    bundle->member_tp_attached = PJ_TRUE;
    return PJ_SUCCESS;
}

static pj_status_t transport_endpoint_get_info(pjmedia_transport *tp,
				      pjmedia_transport_info *info)
{
    transport_bundle_endpoint *endpoint = (transport_bundle_endpoint*) tp;
    PJ_ASSERT_RETURN(tp, PJ_EINVAL);
    return transport_get_info((pjmedia_transport*)endpoint->bundle, info);
}

static void transport_endpoint_detach(pjmedia_transport *tp, void *strm)
{
    transport_bundle_endpoint *endpoint = (transport_bundle_endpoint*) tp;

    PJ_UNUSED_ARG(strm);
    PJ_ASSERT_ON_FAIL(tp, return);

    transport_detach((pjmedia_transport*) endpoint->bundle, endpoint);	
}

static pj_status_t transport_endpoint_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    transport_bundle_endpoint *endpoint = (transport_bundle_endpoint*) tp;  
    PJ_ASSERT_RETURN(tp, PJ_EINVAL);  
    return transport_send_rtp((pjmedia_transport*) endpoint->bundle, pkt, size);
}

static pj_status_t transport_endpoint_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    return transport_endpoint_send_rtcp2(tp, NULL, 0, pkt, size);
}

static pj_status_t transport_endpoint_send_rtcp2(pjmedia_transport *tp,
				        const pj_sockaddr_t *addr,
				        unsigned addr_len,
				        const void *pkt,
				        pj_size_t size)
{
   transport_bundle_endpoint *endpoint = (transport_bundle_endpoint*) tp;
   PJ_ASSERT_RETURN(tp, PJ_EINVAL);
   return transport_send_rtcp((pjmedia_transport*) endpoint->bundle, pkt, size);
}

static pj_status_t transport_endpoint_simulate_lost(pjmedia_transport *tp,
					   pjmedia_dir dir,
					   unsigned pct_lost)
{
    transport_bundle_endpoint *endpoint = (transport_bundle_endpoint *) tp;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    return transport_simulate_lost((pjmedia_transport*) endpoint->bundle, dir, pct_lost);
}


static pj_status_t transport_endpoint_media_create(pjmedia_transport *tp,
				          pj_pool_t *sdp_pool,
					  unsigned options,
				          const pjmedia_sdp_session *sdp_remote,
					  unsigned media_index)
{	
    struct transport_bundle_endpoint *endpoint = (struct transport_bundle_endpoint*) tp;
    pj_status_t status;
    status = transport_media_create((pjmedia_transport*) endpoint->bundle, sdp_pool,
					    options, sdp_remote,
					    media_index);		
	return status;
}


static pj_status_t transport_endpoint_encode_sdp(pjmedia_transport *tp,
					pj_pool_t *sdp_pool,
					pjmedia_sdp_session *sdp_local,
					const pjmedia_sdp_session *sdp_remote,
					unsigned media_index)
{
    
    struct transport_bundle_endpoint *endpoint = (struct transport_bundle_endpoint*) tp;
    pj_status_t status;

    PJ_ASSERT_RETURN(tp && sdp_pool && sdp_local, PJ_EINVAL);
	
	status = transport_encode_sdp((pjmedia_transport*) endpoint->bundle, sdp_pool,
				  sdp_local, sdp_remote, media_index);
	
    return status;
}


static pj_status_t transport_endpoint_media_start(pjmedia_transport *tp,
				         pj_pool_t *pool,
				         const pjmedia_sdp_session *sdp_local,
				         const pjmedia_sdp_session *sdp_remote,
				         unsigned media_index)
{
    struct transport_bundle_endpoint *endpoint = (struct transport_bundle_endpoint*) tp;
    pj_status_t status;

    PJ_ASSERT_RETURN(tp && pool && sdp_local && sdp_remote, PJ_EINVAL);

    status = transport_media_start((pjmedia_transport*) endpoint->bundle, pool,
					   sdp_local, sdp_remote,
				           media_index);    
				          			           
    return status;
}

static pj_status_t transport_endpoint_media_stop(pjmedia_transport *tp)
{
    struct transport_bundle_endpoint *endpoint = (struct transport_bundle_endpoint*) tp;
    pj_status_t status;


    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    /* Invoke media_stop() of member tp */
    status = pjmedia_transport_media_stop((pjmedia_transport*) endpoint->bundle);
		  
    		   
    return status;
}

static pj_status_t transport_endpoint_attach2(pjmedia_transport *tp,
				     pjmedia_transport_attach_param *param)
{
    transport_bundle_endpoint *endpoint = (transport_bundle_endpoint*) tp;

    fprintf(stderr, "ENDPOINT ATTACH2!\n");
    
    endpoint->user_data = param->user_data;
    endpoint->rtp_cb = param->rtp_cb;
    endpoint->rtcp_cb = param->rtcp_cb;
    
	transport_attach2((pjmedia_transport*) endpoint->bundle, param);
        
    return PJ_SUCCESS;
}

static pj_status_t transport_endpoint_destroy  (pjmedia_transport *tp) {
    struct transport_bundle_endpoint *endpoint = (struct transport_bundle_endpoint*) tp;
   // pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);
   
    pj_lock_acquire(endpoint->bundle->mutex); 
   
    for(i = 0; i < MAX_BUNDLE_STREAMS; i++) {
	    if(endpoint->bundle->endpoints[i] == endpoint) endpoint->bundle->endpoints[i] = NULL;
	}
	
	endpoint->bundle = NULL;
    
    pj_lock_release(endpoint->bundle->mutex);

    return PJ_SUCCESS;
}


