/* extended.c - ldap backend extended routines */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2003-2005 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software and subsequently enhanced by Pierangelo
 * Masarati. 
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "slap.h"
#include "back-ldap.h"
#include "lber_pvt.h"

BI_op_extended ldap_back_exop_passwd;

static struct exop {
	struct berval	oid;
	BI_op_extended	*extended;
} exop_table[] = {
	{ BER_BVC(LDAP_EXOP_MODIFY_PASSWD), ldap_back_exop_passwd },
	{ BER_BVNULL, NULL }
};

int
ldap_back_extended(
		Operation	*op,
		SlapReply	*rs )
{
	int	i;

	for ( i = 0; exop_table[i].extended != NULL; i++ ) {
		if ( bvmatch( &exop_table[i].oid, &op->oq_extended.rs_reqoid ) )
		{
			ldapconn_t	*lc;
			LDAPControl	**oldctrls = NULL;
			int		rc;

			/* FIXME: this needs to be called here, so it is
			 * called twice; maybe we could avoid the 
			 * ldap_back_dobind() call inside each extended()
			 * call ... */
			lc = ldap_back_getconn( op, rs, LDAP_BACK_SENDERR );
			if ( !lc || !ldap_back_dobind( lc, op, rs, LDAP_BACK_SENDERR ) ) {
				return -1;
			}

			oldctrls = op->o_ctrls;
			if ( ldap_back_proxy_authz_ctrl( lc, op, rs,
						&op->o_ctrls ) )
			{
				op->o_ctrls = oldctrls;
				send_ldap_result( op, rs );
				rs->sr_text = NULL;
				rc = rs->sr_err;
				goto done;
			}

			rc = ( *exop_table[i].extended )( op, rs );

			if ( op->o_ctrls && op->o_ctrls != oldctrls ) {
				free( op->o_ctrls[ 0 ] );
				free( op->o_ctrls );
			}
			op->o_ctrls = oldctrls;

done:;
			if ( lc != NULL ) {
				ldap_back_release_conn( op, rs, lc );
			}
			
			return rc;
		}
	}

	rs->sr_text = "not supported within naming context";
	return LDAP_UNWILLING_TO_PERFORM;
}

int
ldap_back_exop_passwd(
		Operation	*op,
		SlapReply	*rs )
{
	ldapconn_t	*lc;
	req_pwdexop_s	*qpw = &op->oq_pwdexop;
	LDAPMessage	*res;
	ber_int_t	msgid;
	int		rc, isproxy;
	int		do_retry = 1;

	lc = ldap_back_getconn( op, rs, LDAP_BACK_SENDERR );
	if ( !lc || !ldap_back_dobind( lc, op, rs, LDAP_BACK_SENDERR ) ) {
		return -1;
	}

	isproxy = ber_bvcmp( &op->o_req_ndn, &op->o_ndn );

	Debug( LDAP_DEBUG_ARGS, "==> ldap_back_exop_passwd(\"%s\")%s\n",
		op->o_req_dn.bv_val, isproxy ? " (proxy)" : "", 0 );

retry:
	rc = ldap_passwd( lc->lc_ld, isproxy ? &op->o_req_dn : NULL,
		qpw->rs_old.bv_val ? &qpw->rs_old : NULL,
		qpw->rs_new.bv_val ? &qpw->rs_new : NULL,
		op->o_ctrls, NULL, &msgid );

	if ( rc == LDAP_SUCCESS ) {
		if ( ldap_result( lc->lc_ld, msgid, LDAP_MSG_ALL, NULL, &res ) == -1 ) {
			ldap_get_option( lc->lc_ld, LDAP_OPT_ERROR_NUMBER, &rc );
			ldap_back_freeconn( op, lc, 0 );
			lc = NULL;

		} else {
			/* sigh. parse twice, because parse_passwd
			 * doesn't give us the err / match / msg info.
			 */
			rc = ldap_parse_result( lc->lc_ld, res, &rs->sr_err,
					(char **)&rs->sr_matched,
					(char **)&rs->sr_text,
					NULL, NULL, 0 );
#ifndef LDAP_NULL_IS_NULL
			if ( rs->sr_matched && rs->sr_matched[ 0 ] == '\0' ) {
				free( (char *)rs->sr_matched );
				rs->sr_matched = NULL;
			}
			if ( rs->sr_text && rs->sr_text[ 0 ] == '\0' ) {
				free( (char *)rs->sr_text );
				rs->sr_text = NULL;
			}
#endif /* LDAP_NULL_IS_NULL */
			if ( rc == LDAP_SUCCESS ) {
				if ( rs->sr_err == LDAP_SUCCESS ) {
					struct berval	newpw;
					
					rc = ldap_parse_passwd( lc->lc_ld, res,
							&newpw);
					if ( rc == LDAP_SUCCESS &&
							!BER_BVISNULL( &newpw ) )
					{
						rs->sr_type = REP_EXTENDED;
						rs->sr_rspdata = slap_passwd_return( &newpw );
						free( newpw.bv_val );
					}

				} else {
					rc = rs->sr_err;
				}
			}
			ldap_msgfree( res );
		}
	}
	if ( rc != LDAP_SUCCESS ) {
		rs->sr_err = slap_map_api2result( rs );
		if ( rs->sr_err == LDAP_UNAVAILABLE && do_retry ) {
			do_retry = 0;
			if ( ldap_back_retry( &lc, op, rs, LDAP_BACK_SENDERR ) ) {
				goto retry;
			}
		}
		send_ldap_result( op, rs );
		rc = -1;
	}

	/* these have to be freed anyway... */
	if ( rs->sr_matched ) {
		free( (char *)rs->sr_matched );
		rs->sr_matched = NULL;
	}
	if ( rs->sr_text ) {
		free( (char *)rs->sr_text );
		rs->sr_text = NULL;
	}

	if ( lc != NULL ) {
		ldap_back_release_conn( op, rs, lc );
	}

	return rc;
}
