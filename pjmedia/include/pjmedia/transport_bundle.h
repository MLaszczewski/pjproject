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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#ifndef __PJMEDIA_TRANSPORT_BUNDLE_H__
#define __PJMEDIA_TRANSPORT_BUNDLE_H__

/**
 * @file transport_bundle.h
 * @brief bundle media transport.
 */

#include <pjmedia/stream.h>
#include <pjmedia/transport.h>

PJ_BEGIN_DECL

/**
 * Create an bundle media transport.
 *
 * @param endpt	    The media endpoint instance.
 * @param tp	    The actual media transport to send and receive 
 *		    RTP packets. This media transport will be
 *		    kept as member transport of this BUNDLE instance.
 * @param p_tp	    Pointer to receive the transport BUNDLE instance.
 *
 * @return	    PJ_SUCCESS on success.
 */
PJ_DECL(pj_status_t) pjmedia_transport_bundle_create(
				       pjmedia_endpt *endpt,
				       pjmedia_transport *tp,				       
				       pjmedia_transport **p_tp);
				       
/**
 * Create an bundle endpoint
 * 
 * @param tp        BUNDLE transport
 * @param ssrc      source identifier
 * @param p_tpe     Pointer to receive endpoint
 * @return          PJ_SUCCESS on success
 */			       
PJ_DECL(pj_status_t) pjmedia_transport_bundle_endpoint_create(
				       pjmedia_transport *tp,
				       pj_uint32_t ssrc,
				       pj_uint32_t rem_ssrc,
				       pjmedia_transport **p_tpe);
				       
PJ_END_DECL



#endif	/* __PJMEDIA_TRANSPORT_MUX_H__ */
