/*  =========================================================================
    bios_alerts_list_server - Providing information about active and resolved alerts

    Copyright (C) 2014 - 2015 Eaton                                        
                                                                           
    This program is free software; you can redistribute it and/or modify   
    it under the terms of the GNU General Public License as published by   
    the Free Software Foundation; either version 2 of the License, or      
    (at your option) any later version.                                    
                                                                           
    This program is distributed in the hope that it will be useful,        
    but WITHOUT ANY WARRANTY; without even the implied warranty of         
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
    GNU General Public License for more details.                           
                                                                           
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            
    =========================================================================
*/

/*
@header
    bios_alerts_list_server - Providing information about active and resolved alerts
@discuss
@end
*/
#include "../include/alert_list.h"

#define RFC_ALERTS_LIST_SUBJECT "rfc-alerts-list"

static int
alert_id_comparator (bios_proto_t *alert1, bios_proto_t *alert2) {
    assert (alert1);
    assert (alert2);
    assert (bios_proto_id (alert1) == BIOS_PROTO_ALERT);
    assert (bios_proto_id (alert2) == BIOS_PROTO_ALERT);

    if (streq (bios_proto_rule (alert1), bios_proto_rule (alert2)) &&
        streq (bios_proto_element_src (alert1), bios_proto_element_src (alert2))) {
        return 0;
    }
    else {
        return 1;
    }
}

static int
alert_comparator (bios_proto_t *alert1, bios_proto_t *alert2) {
    assert (alert1);
    assert (alert2);
    assert (bios_proto_id (alert1) == BIOS_PROTO_ALERT);
    assert (bios_proto_id (alert2) == BIOS_PROTO_ALERT);

    if (streq (bios_proto_rule (alert1), bios_proto_rule (alert2)) &&
        streq (bios_proto_element_src (alert1), bios_proto_element_src (alert2)) &&
        streq (bios_proto_state (alert1), bios_proto_state (alert2)) &&
        streq (bios_proto_severity (alert1), bios_proto_severity (alert2)) &&
        streq (bios_proto_description (alert1), bios_proto_description (alert2)) &&
        streq (bios_proto_action (alert1), bios_proto_action (alert2)) &&
        bios_proto_time (alert1) == bios_proto_time (alert2)) {
        return 0;
    }
    else {
        return 1;
    }
}

static int
is_alertstate (const char *state) {
    assert (state);
    if (streq (state, "ALL") ||
        streq (state, "ACTIVE") ||
        streq (state, "ACK-WIP") ||
        streq (state, "ACK-IGNORE") ||
        streq (state, "ACK-PAUSE") ||
        streq (state, "ACK-SILENCE")) {
        return 1;
    }
    return 0;
}

static void
s_handle_stream_deliver (zmsg_t** msg_p, zlistx_t *alerts) {
    assert (msg_p);
    zmsg_t *msg = *msg_p;
    bios_proto_t *alert = bios_proto_decode (&msg);
    if (!alert || bios_proto_id (alert) != BIOS_PROTO_ALERT) {
        zsys_warning ("s_handle_stream_deliver (): Message not BIOS_PROTO_ALERT.");
        return;
    }

    bios_proto_t *cursor = (bios_proto_t *) zlistx_first (alerts);
    if (cursor) {
        int found = 0;
        while (cursor) {
            if (alert_id_comparator (cursor, alert) == 0) {
                if (streq (bios_proto_state (alert), "RESOLVED")) {
                    zlistx_delete (alerts, zlistx_cursor (alerts));
                }
                else {
                    bios_proto_set_state (cursor, "%s", bios_proto_state (alert));
                    bios_proto_set_severity (cursor, "%s", bios_proto_severity (alert));
                    bios_proto_set_description (cursor, "%s", bios_proto_description (alert));
                    bios_proto_set_action (cursor, "%s", bios_proto_action (alert));
                    bios_proto_set_time (cursor, bios_proto_time (alert));
                }
                found = 1;
                break;
            }
            cursor = (bios_proto_t *) zlistx_next (alerts);
        }
        if (!found) {        
            zlistx_add_end (alerts, alert);
        }
    }
    else if (!streq (bios_proto_state (alert), "RESOLVED")) {
        zlistx_add_end (alerts, alert);
    }
    bios_proto_destroy (&alert);
}

static void
s_send_error_response (mlm_client_t *client, const char *reason) {
    assert (client);
    assert (reason);
    zmsg_t *reply  = zmsg_new ();
    zmsg_addstr (reply, "ERROR");
    zmsg_addstr (reply, reason);
    if (mlm_client_sendto (client, mlm_client_sender (client), RFC_ALERTS_LIST_SUBJECT, NULL, 5000, &reply) != 0) {
        zsys_error ("mlm_client_sendto (sender = '%s', subject = '%s', timeout = '5000') failed.",
                mlm_client_sender (client), RFC_ALERTS_LIST_SUBJECT);
    }
    return;
} 

static void
s_handle_mailbox_deliver (mlm_client_t *client, zmsg_t** msg_p, zlistx_t *alerts) {
    assert (msg_p);
    zmsg_t *msg = *msg_p;
    if (!msg)
        return;
    char *part = zmsg_popstr (msg);
    if (!part || !streq (part, "LIST")) {
        free (part); part = NULL;
        zmsg_destroy (&msg);
        s_send_error_response (client, "BAD_MESSAGE");
        return;
    }
    free (part); part = NULL;
    part = zmsg_popstr (msg);
    zmsg_destroy (&msg);
    if (!is_alertstate (part)) {
        free (part); part = NULL;
        s_send_error_response (client, "NOT_FOUND");
        return;
    } 

    zmsg_t *reply = zmsg_new ();
    zmsg_addstr (reply, "LIST");
    zmsg_addstr (reply, part);
    bios_proto_t *cursor = (bios_proto_t *) zlistx_first (alerts);
    while (cursor) {
        if (streq (part, "ALL") || streq (part, bios_proto_state (cursor))) {
            byte *buffer = NULL;
            bios_proto_t *duplicate = bios_proto_dup (cursor);
            zmsg_t *result = bios_proto_encode (&duplicate);
            size_t nbytes = zmsg_encode (result, &buffer);
            zframe_t *frame = zframe_new ((void *) buffer, nbytes);
            zmsg_destroy (&result);
            free (buffer); buffer = NULL;
            zmsg_append (reply, &frame);
        }
        cursor = (bios_proto_t *) zlistx_next (alerts);
    }
    if (mlm_client_sendto (client, mlm_client_sender (client), RFC_ALERTS_LIST_SUBJECT, NULL, 5000, &reply) != 0) {
        zsys_error ("mlm_client_sendto (sender = '%s', subject = '%s', timeout = '5000') failed.",
                mlm_client_sender (client), RFC_ALERTS_LIST_SUBJECT);
    }
    free (part); part = NULL;
}

void
bios_alerts_list_server (zsock_t *pipe, void *args)
{
    static const char* endpoint = "inproc://bios-lm-server-test";
    zlistx_t *alerts = zlistx_new ();
    zlistx_set_destructor (alerts, (czmq_destructor *) bios_proto_destroy);
    zlistx_set_duplicator (alerts, (czmq_duplicator *) bios_proto_dup);

    mlm_client_t *client = mlm_client_new ();
    mlm_client_connect (client, endpoint, 1000, "ALERTS-LIST");
    mlm_client_set_consumer (client, "ALERTS", ".*");

    zpoller_t *poller = zpoller_new (pipe, mlm_client_msgpipe (client), NULL);
    zsock_signal (pipe, 0);

    while (!zsys_interrupted) {

        void *which = zpoller_wait (poller, -1);
        if (which == pipe) {
            break;
        }

        zmsg_t *msg = mlm_client_recv (client);
        if (!msg)
            break;
        if (streq (mlm_client_command (client), "MAILBOX DELIVER")) {
            if (streq (mlm_client_subject (client), RFC_ALERTS_LIST_SUBJECT)) {
                s_handle_mailbox_deliver (client, &msg, alerts);
            }
            else {
                zsys_warning ("Unknown protocol. Subject: '%s', Sender: '%s'.",
                    mlm_client_subject (client), mlm_client_sender (client));
            }
        }
        else if (streq (mlm_client_command (client), "STREAM DELIVER")) {
            s_handle_stream_deliver (&msg, alerts);
        }
        else {
            zsys_warning ("Unknown command '%s'. Subject: '%s', Sender: '%s'.",
                    mlm_client_command (client), mlm_client_subject (client), mlm_client_sender (client));
            zmsg_destroy (&msg);
        }
    }

    zpoller_destroy (&poller);
    mlm_client_destroy (&client);
    zlistx_destroy (&alerts);
}

//  --------------------------------------------------------------------------
//  Self test of this class.

// ---- Test Helper Functions

static zmsg_t *
test_request_alerts_list (mlm_client_t *user_interface, const char *state) {
    assert (user_interface);
    assert (state);
    assert (is_alertstate (state));
    zmsg_t *send = zmsg_new ();
    zmsg_addstr (send, "LIST");
    zmsg_addstr (send, state);
    if (mlm_client_sendto (user_interface, "ALERTS-LIST", RFC_ALERTS_LIST_SUBJECT, NULL, 5000, &send) != 0) {
        zmsg_destroy (&send);
        zsys_error ("mlm_client_sendto (address = 'ALERTS-LIST', subject = 'rfc-alerts-list') failed.");
        return NULL;
    }
    zmsg_t *reply = mlm_client_recv (user_interface);
    assert (streq (mlm_client_command (user_interface), "MAILBOX DELIVER"));
    assert (streq (mlm_client_sender (user_interface), "ALERTS-LIST"));
    assert (streq (mlm_client_subject (user_interface), RFC_ALERTS_LIST_SUBJECT));
    assert (reply);
    return reply;
}

static int
test_zlistx_same (const char *state, zlistx_t *expected, zlistx_t *received) {
    assert (state);
    assert (expected);
    assert (received);
    bios_proto_t *cursor = (bios_proto_t *) zlistx_first (expected);
    while (cursor) {
        if (streq (state, "ALL") || streq (state, bios_proto_state (cursor))) {
            void *handle = zlistx_find (received, cursor);
            if (!handle)
                return 0;
            zlistx_delete (received, handle);
        }
        cursor = (bios_proto_t *) zlistx_next (expected);
    }
    if (zlistx_size (received) != 0)
        return 0;
    return 1;
}

static void
test_check_result (const char *state, zlistx_t *expected, zmsg_t **reply_p, int fail) {
    assert (state);
    assert (expected);
    assert (reply_p);
    if (!*reply_p)
        return;
    zmsg_t *reply = *reply_p;
    // check leading protocol frames (strings)
    char *part = zmsg_popstr (reply);
    assert (streq (part, "LIST"));
    free (part); part = NULL;
    part = zmsg_popstr (reply);
    assert (streq (part, state));
    free (part); part = NULL;

    zlistx_t *received = zlistx_new ();
    zlistx_set_destructor (received, (czmq_destructor *) bios_proto_destroy);
    zlistx_set_duplicator (received, (czmq_duplicator *) bios_proto_dup);
    zlistx_set_comparator (received, (czmq_comparator *) alert_comparator);
    zframe_t *frame = zmsg_pop (reply);
    while (frame) {
        zmsg_t *decoded_zmsg = zmsg_decode (zframe_data (frame), zframe_size (frame));
        zframe_destroy (&frame);
        assert (decoded_zmsg);
        bios_proto_t *decoded = bios_proto_decode (&decoded_zmsg);
        assert (decoded);
        assert (bios_proto_id (decoded) == BIOS_PROTO_ALERT);
        zlistx_add_end (received, decoded);
        bios_proto_destroy (&decoded);
        frame = zmsg_pop (reply);        
    }
    // compare the two by iterative substraction
    int rv = test_zlistx_same (state, expected, received);
    if (fail)
        assert (rv == 0);
    else
        assert (rv == 1);
    zlistx_destroy (&received);
    zmsg_destroy (reply_p); 
}

static bios_proto_t *
test_alert_new (const char *rule,
                const char *element,
                const char *state,
                const char *severity,
                const char *description,
                uint64_t time,
                const char *action
                ) {
    bios_proto_t *alert = bios_proto_new (BIOS_PROTO_ALERT);
    assert (alert);
    bios_proto_set_rule (alert, rule);
    bios_proto_set_element_src (alert, element);
    bios_proto_set_state (alert, state);
    bios_proto_set_severity (alert, severity);
    bios_proto_set_description (alert, description);
    bios_proto_set_action (alert, action);
    bios_proto_set_time (alert, time);
    return alert;
}

static void
test_alert_copy (bios_proto_t *to, bios_proto_t *from) {
    assert (to);
    assert (from);
    bios_proto_set_id (to, BIOS_PROTO_ALERT);
    assert (bios_proto_id (from) == BIOS_PROTO_ALERT);

    bios_proto_set_rule (to, bios_proto_rule (from));
    bios_proto_set_element_src (to, bios_proto_element_src (from));
    bios_proto_set_state (to, bios_proto_state (from));
    bios_proto_set_severity (to, bios_proto_severity (from));
    bios_proto_set_description (to, bios_proto_description (from));
    bios_proto_set_action (to, bios_proto_action (from));
    bios_proto_set_time (to, bios_proto_time (from));
}

static void
test_alert_publish (mlm_client_t *alert_producer, zlistx_t *alerts, bios_proto_t **message) {
    assert (message);
    assert (*message);
    assert (alerts);
    assert (alert_producer);

    void *handle = zlistx_find (alerts, (void *) *message);
    if (handle) {
        bios_proto_t *item = (bios_proto_t *) zlistx_handle_item (handle);
        if (streq (bios_proto_state (*message), "RESOLVED")) {
            zlistx_delete (alerts, handle);
        }
        else {
            test_alert_copy (item, *message);
        }
    }
    else {
        zlistx_add_end (alerts, *message);
    }

    zmsg_t *zmessage = bios_proto_encode (message);
    assert (zmessage);
    int rv = mlm_client_send (alert_producer, "Nobody here cares about this.", &zmessage);
    assert (rv == 0);
    zclock_sleep (500);
   
}


void
bios_alerts_list_server_test (bool verbose)
{

    static const char* endpoint = "inproc://bios-lm-server-test";

    // Malamute
    zactor_t *server = zactor_new (mlm_server, "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    if (verbose)
        zstr_send (server, "VERBOSE");

    // User Interface
    mlm_client_t *ui_client = mlm_client_new ();
    mlm_client_connect (ui_client, endpoint, 1000, "UI");

    // Alert Producer
    mlm_client_t *ap_client = mlm_client_new ();
    mlm_client_connect (ap_client, endpoint, 1000, "ALERTS-PUB");
    mlm_client_set_producer (ap_client, "ALERTS");

    // Alert List
    zactor_t *bios_al_server = zactor_new (bios_alerts_list_server, NULL);

    //  @selftest
    //  Simple create/destroy test

    // maintain a list of active alerts (that serves as "expected results")
    zlistx_t *alerts = zlistx_new ();
    zlistx_set_destructor (alerts, (czmq_destructor *) bios_proto_destroy);
    zlistx_set_duplicator (alerts, (czmq_duplicator *) bios_proto_dup);
    zlistx_set_comparator (alerts, (czmq_comparator *) alert_id_comparator);

    zmsg_t *reply = test_request_alerts_list (ui_client, "ALL");
    assert (reply);
    test_check_result ("ALL", alerts, &reply, 0);

    reply = test_request_alerts_list (ui_client, "ACK-WIP");
    test_check_result ("ACK-WIP", alerts, &reply, 0);
    
    reply = test_request_alerts_list (ui_client, "ACK-IGNORE");
    test_check_result ("ACK-IGNORE", alerts, &reply, 0);
    
    // add new alert
    bios_proto_t *alert = test_alert_new ("Threshold", "ups", "ACTIVE", "high", "description", 1, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    reply = test_request_alerts_list (ui_client, "ACK-PAUSE");
    test_check_result ("ACK-PAUSE", alerts, &reply, 0);
    
    // add new alert
    alert = test_alert_new ("Threshold", "epdu", "ACTIVE", "high", "description", 2, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    // add new alert
    alert = test_alert_new ("SimpleRule", "ups", "ACTIVE", "high", "description", 3, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    // add new alert
    alert = test_alert_new ("SimpleRule", "karolkovo", "ACTIVE", "high", "description", 4, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    reply = test_request_alerts_list (ui_client, "ACTIVE");
    test_check_result ("ACTIVE", alerts, &reply, 0);

    // change state 
    alert = test_alert_new ("Threshold", "epdu", "ACK-WIP", "high", "description", 5, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    // change state back
    alert = test_alert_new ("Threshold", "epdu", "ACTIVE", "high", "description", 6, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    // change state of two alerts
    alert = test_alert_new ("Threshold", "ups", "ACK-PAUSE", "high", "description", 7, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    alert = test_alert_new ("SimpleRule", "ups", "ACK-PAUSE", "high", "description", 8, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    reply = test_request_alerts_list (ui_client, "ACK-PAUSE");
    test_check_result ("ACK-PAUSE", alerts, &reply, 0);

    alert = test_alert_new ("SimpleRule", "ups", "ACK-WIP", "high", "description", 9, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
 
    reply = test_request_alerts_list (ui_client, "ACK-WIP");
    test_check_result ("ACK-WIP", alerts, &reply, 0);

    // resolve alert
    alert = test_alert_new ("SimpleRule", "karolkovo", "RESOLVED", "high", "description", 10, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);
    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 0);

    // Now, let's publish an alert as-a-byspass (i.e. we don't add it to expected)
    // and expect a failure (i.e. expected list != received list)
    zmsg_t *alert_bypass = bios_proto_encode_alert (NULL, "Pattern", "rack", "ACTIVE", "high", "description", 11, "EMAIL|SMS");
    mlm_client_send (ap_client, "Nobody cares", &alert_bypass);
    zclock_sleep (500);

    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 1);

    reply = test_request_alerts_list (ui_client, "ACTIVE");
    test_check_result ("ACTIVE", alerts, &reply, 1);

    alert_bypass = bios_proto_encode_alert (NULL, "Pattern", "rack", "ACK-WIP", "high", "description", 12, "EMAIL|SMS");
    mlm_client_send (ap_client, "Nobody cares", &alert_bypass);
    zclock_sleep (500);

    reply = test_request_alerts_list (ui_client, "ALL");
    test_check_result ("ALL", alerts, &reply, 1);
    
    reply = test_request_alerts_list (ui_client, "ACK-WIP");
    test_check_result ("ACK-WIP", alerts, &reply, 1);

    // when requesting a different type, it should be ok though
    alert = test_alert_new ("BlackBooks", "store", "ACK-PAUSE", "high", "description", 13, "EMAIL|SMS");
    test_alert_publish (ap_client, alerts, &alert);

    reply = test_request_alerts_list (ui_client, "ACK-PAUSE");
    test_check_result ("ACK-PAUSE", alerts, &reply, 0);

    // Now, let's test an error response
    zmsg_t *send = zmsg_new ();
    zmsg_addstr (send, "LIST");
    zmsg_addstr (send, "RESOLVED");
    int rv = mlm_client_sendto (ui_client, "ALERTS-LIST", RFC_ALERTS_LIST_SUBJECT, NULL, 5000, &send);
    assert (rv == 0);
    reply = mlm_client_recv (ui_client);
    assert (streq (mlm_client_command (ui_client), "MAILBOX DELIVER"));
    assert (streq (mlm_client_sender (ui_client), "ALERTS-LIST"));
    assert (streq (mlm_client_subject (ui_client), RFC_ALERTS_LIST_SUBJECT));   
    char *part = zmsg_popstr (reply);
    assert (streq (part, "ERROR"));
    free (part); part = NULL;
    part = zmsg_popstr (reply);
    assert (streq (part, "NOT_FOUND"));
    free (part); part = NULL;

    send = zmsg_new ();
    zmsg_addstr (send, "LIST");
    zmsg_addstr (send, "Karolino");
    rv = mlm_client_sendto (ui_client, "ALERTS-LIST", RFC_ALERTS_LIST_SUBJECT, NULL, 5000, &send);
    assert (rv == 0);
    reply = mlm_client_recv (ui_client);
    assert (streq (mlm_client_command (ui_client), "MAILBOX DELIVER"));
    assert (streq (mlm_client_sender (ui_client), "ALERTS-LIST"));
    assert (streq (mlm_client_subject (ui_client), RFC_ALERTS_LIST_SUBJECT));   
    part = zmsg_popstr (reply);
    assert (streq (part, "ERROR"));
    free (part); part = NULL;
    part = zmsg_popstr (reply);
    assert (streq (part, "NOT_FOUND"));
    free (part); part = NULL;

    send = zmsg_new ();
    zmsg_addstr (send, "Hatatitla");
    zmsg_addstr (send, "Karolino");
    rv = mlm_client_sendto (ui_client, "ALERTS-LIST", RFC_ALERTS_LIST_SUBJECT, NULL, 5000, &send);
    assert (rv == 0);
    reply = mlm_client_recv (ui_client);
    assert (streq (mlm_client_command (ui_client), "MAILBOX DELIVER"));
    assert (streq (mlm_client_sender (ui_client), "ALERTS-LIST"));
    assert (streq (mlm_client_subject (ui_client), RFC_ALERTS_LIST_SUBJECT));   
    part = zmsg_popstr (reply);
    assert (streq (part, "ERROR"));
    free (part); part = NULL;
    part = zmsg_popstr (reply);
    assert (part);
    free (part); part = NULL;

    mlm_client_destroy (&ui_client);
    mlm_client_destroy (&ap_client);
   
    zactor_destroy (&bios_al_server);
    zactor_destroy (&server);
    printf ("OK\n");
}
