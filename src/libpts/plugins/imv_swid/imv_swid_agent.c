/*
 * Copyright (C) 2013 Andreas Steffen
 * HSR Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "imv_swid_agent.h"
#include "imv_swid_state.h"

#include "libpts.h"
#include "tcg/swid/tcg_swid_attr_req.h"
#include "tcg/swid/tcg_swid_attr_tag_id_inv.h"

#include <imcv.h>
#include <imv/imv_agent.h>
#include <imv/imv_msg.h>

#include <tncif_names.h>
#include <tncif_pa_subtypes.h>

#include <pen/pen.h>
#include <utils/debug.h>

typedef struct private_imv_swid_agent_t private_imv_swid_agent_t;

/* Subscribed PA-TNC message subtypes */
static pen_type_t msg_types[] = {
	{ PEN_TCG, PA_SUBTYPE_TCG_SWID }
};

/**
 * Private data of an imv_swid_agent_t object.
 */
struct private_imv_swid_agent_t {

	/**
	 * Public members of imv_swid_agent_t
	 */
	imv_agent_if_t public;

	/**
	 * IMV agent responsible for generic functions
	 */
	imv_agent_t *agent;

};

METHOD(imv_agent_if_t, bind_functions, TNC_Result,
	private_imv_swid_agent_t *this, TNC_TNCS_BindFunctionPointer bind_function)
{
	return this->agent->bind_functions(this->agent, bind_function);
}

METHOD(imv_agent_if_t, notify_connection_change, TNC_Result,
	private_imv_swid_agent_t *this, TNC_ConnectionID id,
	TNC_ConnectionState new_state)
{
	imv_state_t *state;

	switch (new_state)
	{
		case TNC_CONNECTION_STATE_CREATE:
			state = imv_swid_state_create(id);
			return this->agent->create_state(this->agent, state);
		case TNC_CONNECTION_STATE_DELETE:
			return this->agent->delete_state(this->agent, id);
		default:
			return this->agent->change_state(this->agent, id, new_state, NULL);
	}
}

/**
 * Process a received message
 */
static TNC_Result receive_msg(private_imv_swid_agent_t *this,
							  imv_state_t *state, imv_msg_t *in_msg)
{
	imv_msg_t *out_msg;
	imv_session_t *session;
	imv_workitem_t *workitem, *found = NULL;
	enumerator_t *enumerator;
	pa_tnc_attr_t *attr;
	pen_type_t type;
	TNC_IMV_Evaluation_Result eval;
	TNC_IMV_Action_Recommendation rec;
	TNC_Result result;
	char *result_str;
	bool fatal_error = FALSE;

	/* parse received PA-TNC message and handle local and remote errors */
	result = in_msg->receive(in_msg, &fatal_error);
	if (result != TNC_RESULT_SUCCESS)
	{
		return result;
	}

	session = state->get_session(state);

	/* analyze PA-TNC attributes */
	enumerator = in_msg->create_attribute_enumerator(in_msg);
	while (enumerator->enumerate(enumerator, &attr))
	{
		type = attr->get_type(attr);

		if (type.vendor_id != PEN_TCG)
		{
			continue;
		}
		switch (type.type)
		{
			case TCG_SWID_TAG_ID_INVENTORY:
			{
				tcg_swid_attr_tag_id_inv_t *attr_cast;
				u_int32_t request_id;
				swid_tag_id_t *tag_id;
				chunk_t tag_creator, unique_sw_id;
				enumerator_t *et, *ew;

				attr_cast = (tcg_swid_attr_tag_id_inv_t*)attr;
				request_id = attr_cast->get_request_id(attr_cast);

				DBG2(DBG_IMV, "received SWID tag ID inventory for request %d",
							   request_id);
				et = attr_cast->create_tag_id_enumerator(attr_cast);
				while (et->enumerate(et, &tag_id))
				{
					tag_creator = tag_id->get_tag_creator(tag_id);
					unique_sw_id = tag_id->get_unique_sw_id(tag_id, NULL);
					DBG3(DBG_IMV, "  %.*s_%.*s.swidtag",
						 tag_creator.len, tag_creator.ptr,
						 unique_sw_id.len, unique_sw_id.ptr);
				}
				et->destroy(et);

				if (request_id == 0)
				{
					/* TODO handle subscribed messages */
					break;
				}

				ew = session->create_workitem_enumerator(session);
				while (ew->enumerate(ew, &workitem))
				{
					if (workitem->get_id(workitem) == request_id)
					{
						found = workitem;
						break;
					}
				}

				if (!found)
				{
					DBG1(DBG_IMV, "no workitem found for SWID tag ID inventory "
								  "with request ID %d", request_id);
					ew->destroy(ew);
					break;
				}

				eval = TNC_IMV_EVALUATION_RESULT_COMPLIANT;
				result_str = "received SWID tag ID inventory";
				session->remove_workitem(session, ew);
				ew->destroy(ew);
				rec = found->set_result(found, result_str, eval);
				state->update_recommendation(state, rec, eval);
				imcv_db->finalize_workitem(imcv_db, found);
				found->destroy(found);
				break;
			}
			case TCG_SWID_TAG_INVENTORY:
				break;
			default:
				break;
 		}
	}
	enumerator->destroy(enumerator);

	if (fatal_error)
	{
		state->set_recommendation(state,
								TNC_IMV_ACTION_RECOMMENDATION_NO_RECOMMENDATION,
								TNC_IMV_EVALUATION_RESULT_ERROR);
		out_msg = imv_msg_create_as_reply(in_msg);
		result = out_msg->send_assessment(out_msg);
		out_msg->destroy(out_msg);
		if (result != TNC_RESULT_SUCCESS)
		{
			return result;
		}
		return this->agent->provide_recommendation(this->agent, state);
	}

	return TNC_RESULT_SUCCESS;
}

METHOD(imv_agent_if_t, receive_message, TNC_Result,
	private_imv_swid_agent_t *this, TNC_ConnectionID id,
	TNC_MessageType msg_type, chunk_t msg)
{
	imv_state_t *state;
	imv_msg_t *in_msg;
	TNC_Result result;

	if (!this->agent->get_state(this->agent, id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	in_msg = imv_msg_create_from_data(this->agent, state, id, msg_type, msg);
	result = receive_msg(this, state, in_msg);
	in_msg->destroy(in_msg);

	return result;
}

METHOD(imv_agent_if_t, receive_message_long, TNC_Result,
	private_imv_swid_agent_t *this, TNC_ConnectionID id,
	TNC_UInt32 src_imc_id, TNC_UInt32 dst_imv_id,
	TNC_VendorID msg_vid, TNC_MessageSubtype msg_subtype, chunk_t msg)
{
	imv_state_t *state;
	imv_msg_t *in_msg;
	TNC_Result result;

	if (!this->agent->get_state(this->agent, id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	in_msg = imv_msg_create_from_long_data(this->agent, state, id,
					src_imc_id, dst_imv_id, msg_vid, msg_subtype, msg);
	result = receive_msg(this, state, in_msg);
	in_msg->destroy(in_msg);

	return result;

}

METHOD(imv_agent_if_t, batch_ending, TNC_Result,
	private_imv_swid_agent_t *this, TNC_ConnectionID id)
{
	imv_msg_t *out_msg;
	imv_state_t *state;
	imv_session_t *session;
	imv_workitem_t *workitem;
	imv_swid_state_t *swid_state;
	imv_swid_handshake_state_t handshake_state;
	pa_tnc_attr_t *attr;
	TNC_IMVID imv_id;
	TNC_Result result = TNC_RESULT_SUCCESS;
	bool no_workitems = TRUE;
	u_int32_t request_id;
	u_int8_t flags;
	enumerator_t *enumerator;

	if (!this->agent->get_state(this->agent, id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	swid_state = (imv_swid_state_t*)state;
	handshake_state = swid_state->get_handshake_state(swid_state);
	session = state->get_session(state);
	imv_id = this->agent->get_id(this->agent);

	if (handshake_state == IMV_SWID_STATE_END)
	{
		return TNC_RESULT_SUCCESS;
	}

	/* create an empty out message - we might need it */
	out_msg = imv_msg_create(this->agent, state, id, imv_id, TNC_IMCID_ANY,
							 msg_types[0]);

	if (!session)
	{
		DBG2(DBG_IMV, "no workitems available - no evaluation possible");
		state->set_recommendation(state,
							TNC_IMV_ACTION_RECOMMENDATION_ALLOW,
							TNC_IMV_EVALUATION_RESULT_DONT_KNOW);
		result = out_msg->send_assessment(out_msg);
		out_msg->destroy(out_msg);
		swid_state->set_handshake_state(swid_state, IMV_SWID_STATE_END);

		if (result != TNC_RESULT_SUCCESS)
		{
			return result;
		}
		return this->agent->provide_recommendation(this->agent, state);
	}

	if (handshake_state == IMV_SWID_STATE_INIT)
	{
		enumerator = session->create_workitem_enumerator(session);
		if (enumerator)
		{
			while (enumerator->enumerate(enumerator, &workitem))
			{
				if (workitem->get_imv_id(workitem) != TNC_IMVID_ANY ||
					workitem->get_type(workitem) != IMV_WORKITEM_SWID_TAGS)
				{
					continue;
				}
				
				flags = TCG_SWID_ATTR_REQ_FLAG_NONE;
				if (strchr(workitem->get_arg_str(workitem), 'R'))
				{
					flags |= TCG_SWID_ATTR_REQ_FLAG_R;
				}
				if (strchr(workitem->get_arg_str(workitem), 'S'))
				{
					flags |= TCG_SWID_ATTR_REQ_FLAG_S;
				}
				if (strchr(workitem->get_arg_str(workitem), 'C'))
				{
					flags |= TCG_SWID_ATTR_REQ_FLAG_C;
				}
				request_id = workitem->get_id(workitem);

				DBG2(DBG_IMV, "IMV %d issues SWID tag request %d",
						 imv_id, request_id);
				attr = tcg_swid_attr_req_create(flags, request_id, 0);
				out_msg->add_attribute(out_msg, attr);
				workitem->set_imv_id(workitem, imv_id);
				no_workitems = FALSE;
			}
			enumerator->destroy(enumerator);

			if (no_workitems)
			{
				DBG2(DBG_IMV, "IMV %d has no workitems - "
							  "no evaluation requested", imv_id);
				state->set_recommendation(state,
								TNC_IMV_ACTION_RECOMMENDATION_ALLOW,
								TNC_IMV_EVALUATION_RESULT_DONT_KNOW);
			}
			handshake_state = IMV_SWID_STATE_WORKITEMS;
			swid_state->set_handshake_state(swid_state, handshake_state);
		}
	}

	/* finalized all workitems ? */
	if (handshake_state == IMV_SWID_STATE_WORKITEMS &&
		session->get_workitem_count(session, imv_id) == 0)
	{
		result = out_msg->send_assessment(out_msg);
		out_msg->destroy(out_msg);
		swid_state->set_handshake_state(swid_state, IMV_SWID_STATE_END);

		if (result != TNC_RESULT_SUCCESS)
		{
			return result;
		}
		return this->agent->provide_recommendation(this->agent, state);
	}

	/* send non-empty PA-TNC message with excl flag not set */
	if (out_msg->get_attribute_count(out_msg))
	{
		result = out_msg->send(out_msg, FALSE);
	}
	out_msg->destroy(out_msg);

	return result;
}

METHOD(imv_agent_if_t, solicit_recommendation, TNC_Result,
	private_imv_swid_agent_t *this, TNC_ConnectionID id)
{
	imv_state_t *state;

	if (!this->agent->get_state(this->agent, id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	return this->agent->provide_recommendation(this->agent, state);
}

METHOD(imv_agent_if_t, destroy, void,
	private_imv_swid_agent_t *this)
{
	this->agent->destroy(this->agent);
	free(this);
	libpts_deinit();
}

/**
 * Described in header.
 */
imv_agent_if_t *imv_swid_agent_create(const char *name, TNC_IMVID id,
										 TNC_Version *actual_version)
{
	private_imv_swid_agent_t *this;
	imv_agent_t *agent;

	agent = imv_agent_create(name, msg_types, countof(msg_types), id,
							 actual_version);
	if (!agent)
	{
		return NULL;
	}

	INIT(this,
		.public = {
			.bind_functions = _bind_functions,
			.notify_connection_change = _notify_connection_change,
			.receive_message = _receive_message,
			.receive_message_long = _receive_message_long,
			.batch_ending = _batch_ending,
			.solicit_recommendation = _solicit_recommendation,
			.destroy = _destroy,
		},
		.agent = agent,
	);

	libpts_init();

	return &this->public;
}
