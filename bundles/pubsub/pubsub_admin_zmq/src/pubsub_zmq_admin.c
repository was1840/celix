/**
 *Licensed to the Apache Software Foundation (ASF) under one
 *or more contributor license agreements.  See the NOTICE file
 *distributed with this work for additional information
 *regarding copyright ownership.  The ASF licenses this file
 *to you under the Apache License, Version 2.0 (the
 *"License"); you may not use this file except in compliance
 *with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing,
 *software distributed under the License is distributed on an
 *"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 *specific language governing permissions and limitations
 *under the License.
 */

#include <memory.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <pubsub_endpoint.h>
#include <czmq.h>

#include "pubsub_utils.h"
#include "pubsub_zmq_admin.h"
#include "pubsub_psa_zmq_constants.h"
#include "pubsub_zmq_topic_sender.h"
#include "pubsub_zmq_topic_receiver.h"

#define L_DEBUG(...) \
    logHelper_log(psa->log, OSGI_LOGSERVICE_DEBUG, __VA_ARGS__)
#define L_INFO(...) \
    logHelper_log(psa->log, OSGI_LOGSERVICE_INFO, __VA_ARGS__)
#define L_WARN(...) \
    logHelper_log(psa->log, OSGI_LOGSERVICE_WARNING, __VA_ARGS__)
#define L_ERROR(...) \
    logHelper_log(psa->log, OSGI_LOGSERVICE_ERROR, __VA_ARGS__)

struct pubsub_zmq_admin {
    celix_bundle_context_t *ctx;
    log_helper_t *log;
    const char *fwUUID;

    char* ipAddress;
    zactor_t* zmq_auth;

    unsigned int basePort;
    unsigned int maxPort;

    double qosSampleScore;
    double qosControlScore;
    double defaultScore;

    bool verbose;

    struct {
        celix_thread_mutex_t mutex;
        hash_map_t *map; //key = scope:topic key, value = pubsub_zmq_topic_sender_t
    } topicSenders;

    struct {
        celix_thread_mutex_t mutex;
        hash_map_t *map; //key = scope:topic key, value = pubsub_zmq_topic_sender_t
    } topicReceivers;

    struct {
        celix_thread_mutex_t mutex;
        hash_map_t *map; //key = endpoint uuid, value = psa_zmq_connected_endpoint_entry_t
    } discoveredEndpoints;

};

static celix_status_t zmq_getIpAddress(const char* interface, const char** ip);
static celix_status_t pubsub_zmqAdmin_connectEndpointToReceiver(pubsub_zmq_admin_t* psa, pubsub_updmc_topic_receiver_t *receiver, const celix_properties_t *endpoint);
static celix_status_t pubsub_zmqAdmin_disconnectEndpointFromReceiver(pubsub_zmq_admin_t* psa, pubsub_updmc_topic_receiver_t *receiver, const celix_properties_t *endpoint);


pubsub_zmq_admin_t* pubsub_zmqAdmin_create(celix_bundle_context_t *ctx, log_helper_t *logHelper) {
    pubsub_zmq_admin_t *psa = calloc(1, sizeof(*psa));
    psa->ctx = ctx;
    psa->log = logHelper;
    psa->verbose = celix_bundleContext_getPropertyAsBool(ctx, PUBSUB_ZMQ_VERBOSE_KEY, PUBSUB_ZMQ_VERBOSE_DEFAULT);
    psa->fwUUID = celix_bundleContext_getProperty(ctx, OSGI_FRAMEWORK_FRAMEWORK_UUID, NULL);

    const char *ip = celix_bundleContext_getProperty(ctx, PUBSUB_ZMQ_PSA_IP_KEY , NULL);

    if (ip == NULL) {
        //TODO try to get ip from subnet (CIDR)
    }

    if (ip == NULL) {
        //try to get ip from itf
        const char *interface = celix_bundleContext_getProperty(ctx, PUBSUB_ZMQ_PSA_ITF_KEY, NULL);
        zmq_getIpAddress(interface, &ip);
    }

    if (ip == NULL) {
        L_WARN("[PSA_ZMQ] Could not determine IP address for PSA, using default ip (%s)", PUBSUB_ZMQ_DEFAULT_IP);
        ip = PUBSUB_ZMQ_DEFAULT_IP;
    }

    psa->ipAddress = strndup(ip, 1024*1024);
    if (psa->verbose) {
        L_INFO("[PSA_ZMQ] Using %s for service annunciation", ip);
    }


    long basePort = celix_bundleContext_getPropertyAsLong(ctx, PSA_ZMQ_BASE_PORT, PSA_ZMQ_DEFAULT_BASE_PORT);
    long maxPort = celix_bundleContext_getPropertyAsLong(ctx, PSA_ZMQ_MAX_PORT, PSA_ZMQ_DEFAULT_MAX_PORT);
    psa->basePort = (unsigned int)basePort;
    psa->maxPort = (unsigned int)maxPort;
    if (psa->verbose) {
        L_INFO("[PSA_ZMQ] Using base till max port: %i till %i", psa->basePort, psa->maxPort);
    }

    // Disable Signal Handling by CZMQ
    setenv("ZSYS_SIGHANDLER", "false", true);

    long nrThreads = celix_bundleContext_getPropertyAsLong(ctx, PUBSUB_ZMQ_NR_THREADS_KEY, 0);
    if (nrThreads > 0) {
        zsys_set_io_threads((size_t)nrThreads);
        L_INFO("[PSA_ZMQ] Using %d threads for ZMQ", (size_t)nrThreads);
    }


#ifdef BUILD_WITH_ZMQ_SECURITY
    // Setup authenticator
    zactor_t* auth = zactor_new (zauth, NULL);
    zstr_sendx(auth, "VERBOSE", NULL);

    // Load all public keys of subscribers into the application
    // This step is done for authenticating subscribers
    char curve_folder_path[MAX_KEY_FOLDER_PATH_LENGTH];
    char* keys_bundle_dir = pubsub_getKeysBundleDir(context);
    snprintf(curve_folder_path, MAX_KEY_FOLDER_PATH_LENGTH, "%s/META-INF/keys/subscriber/public", keys_bundle_dir);
    zstr_sendx (auth, "CURVE", curve_folder_path, NULL);
    free(keys_bundle_dir);

    (*admin)->zmq_auth = auth;
#endif

    psa->defaultScore = celix_bundleContext_getPropertyAsDouble(ctx, PSA_ZMQ_DEFAULT_SCORE_KEY, PSA_ZMQ_DEFAULT_SCORE);
    psa->qosSampleScore = celix_bundleContext_getPropertyAsDouble(ctx, PSA_ZMQ_QOS_SAMPLE_SCORE_KEY, PSA_ZMQ_DEFAULT_QOS_SAMPLE_SCORE);
    psa->qosControlScore = celix_bundleContext_getPropertyAsDouble(ctx, PSA_ZMQ_QOS_CONTROL_SCORE_KEY, PSA_ZMQ_DEFAULT_QOS_CONTROL_SCORE);

    celixThreadMutex_create(&psa->topicSenders.mutex, NULL);
    psa->topicSenders.map = hashMap_create(utils_stringHash, NULL, utils_stringEquals, NULL);

    celixThreadMutex_create(&psa->topicReceivers.mutex, NULL);
    psa->topicReceivers.map = hashMap_create(utils_stringHash, NULL, utils_stringEquals, NULL);

    celixThreadMutex_create(&psa->discoveredEndpoints.mutex, NULL);
    psa->discoveredEndpoints.map = hashMap_create(utils_stringHash, NULL, utils_stringEquals, NULL);

    return psa;
}

void pubsub_zmqAdmin_destroy(pubsub_zmq_admin_t *psa) {
    if (psa == NULL) {
        return;
    }

    //note assuming al psa register services and service tracker are removed.

    celixThreadMutex_lock(&psa->topicSenders.mutex);
    hash_map_iterator_t iter = hashMapIterator_construct(psa->topicSenders.map);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_zmq_topic_sender_t *sender = hashMapIterator_nextValue(&iter);
        pubsub_zmqTopicSender_destroy(sender);
    }
    celixThreadMutex_unlock(&psa->topicSenders.mutex);

    celixThreadMutex_lock(&psa->topicReceivers.mutex);
    iter = hashMapIterator_construct(psa->topicReceivers.map);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_updmc_topic_receiver_t *recv = hashMapIterator_nextValue(&iter);
        pubsub_zmqTopicReceiver_destroy(recv);
    }
    celixThreadMutex_unlock(&psa->topicReceivers.mutex);

    celixThreadMutex_lock(&psa->discoveredEndpoints.mutex);
    iter = hashMapIterator_construct(psa->discoveredEndpoints.map);
    while (hashMapIterator_hasNext(&iter)) {
        celix_properties_t *ep = hashMapIterator_nextValue(&iter);
        celix_properties_destroy(ep);
    }
    celixThreadMutex_unlock(&psa->discoveredEndpoints.mutex);

    celixThreadMutex_destroy(&psa->topicSenders.mutex);
    hashMap_destroy(psa->topicSenders.map, true, false);

    celixThreadMutex_destroy(&psa->topicReceivers.mutex);
    hashMap_destroy(psa->topicReceivers.map, true, false);

    celixThreadMutex_destroy(&psa->discoveredEndpoints.mutex);
    hashMap_destroy(psa->discoveredEndpoints.map, true, false);

    if (psa->zmq_auth != NULL){
        zactor_destroy(&psa->zmq_auth);
    }

    free(psa->ipAddress);

    free(psa);
}

celix_status_t pubsub_zmqAdmin_matchPublisher(void *handle, long svcRequesterBndId, const celix_filter_t *svcFilter, double *outScore, long *outSerializerSvcId) {
    pubsub_zmq_admin_t *psa = handle;
    L_DEBUG("[PSA_ZMQ] pubsub_zmqAdmin_matchPublisher");
    celix_status_t  status = CELIX_SUCCESS;
    double score = pubsub_utils_matchPublisher(psa->ctx, svcRequesterBndId, svcFilter->filterStr, PUBSUB_ZMQ_ADMIN_TYPE,
                                                psa->qosSampleScore, psa->qosControlScore, psa->defaultScore, outSerializerSvcId);
    *outScore = score;

    return status;
}

celix_status_t pubsub_zmqAdmin_matchSubscriber(void *handle, long svcProviderBndId, const celix_properties_t *svcProperties, double *outScore, long *outSerializerSvcId) {
    pubsub_zmq_admin_t *psa = handle;
    L_DEBUG("[PSA_ZMQ] pubsub_zmqAdmin_matchSubscriber");
    celix_status_t  status = CELIX_SUCCESS;
    double score = pubsub_utils_matchSubscriber(psa->ctx, svcProviderBndId, svcProperties, PUBSUB_ZMQ_ADMIN_TYPE,
            psa->qosSampleScore, psa->qosControlScore, psa->defaultScore, outSerializerSvcId);
    if (outScore != NULL) {
        *outScore = score;
    }
    return status;
}

celix_status_t pubsub_zmqAdmin_matchEndpoint(void *handle, const celix_properties_t *endpoint, bool *outMatch) {
    pubsub_zmq_admin_t *psa = handle;
    L_DEBUG("[PSA_ZMQ] pubsub_zmqAdmin_matchEndpoint");
    celix_status_t  status = CELIX_SUCCESS;
    bool match = pubsub_utils_matchEndpoint(psa->ctx, endpoint, PUBSUB_ZMQ_ADMIN_TYPE, NULL);
    if (outMatch != NULL) {
        *outMatch = match;
    }
    return status;
}

celix_status_t pubsub_zmqAdmin_setupTopicSender(void *handle, const char *scope, const char *topic, long serializerSvcId, celix_properties_t **outPublisherEndpoint) {
    pubsub_zmq_admin_t *psa = handle;
    celix_status_t  status = CELIX_SUCCESS;

    //1) Create TopicSender
    //2) Store TopicSender
    //3) Connect existing endpoints
    //4) set outPublisherEndpoint

    celix_properties_t *newEndpoint = NULL;

    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    celixThreadMutex_lock(&psa->topicSenders.mutex);
    pubsub_zmq_topic_sender_t *sender = hashMap_get(psa->topicSenders.map, key);
    if (sender == NULL) {
        sender = pubsub_zmqTopicSender_create(psa->ctx, psa->log, scope, topic, serializerSvcId, psa->ipAddress, psa->basePort, psa->maxPort);
        if (sender != NULL) {
            const char *psaType = pubsub_zmqTopicSender_psaType(sender);
            const char *serType = pubsub_zmqTopicSender_serializerType(sender);
            newEndpoint = pubsubEndpoint_create(psa->fwUUID, scope, topic, PUBSUB_PUBLISHER_ENDPOINT_TYPE, psaType,
                                                serType, NULL);
            celix_properties_set(newEndpoint, PUBSUB_ZMQ_URL_KEY, pubsub_zmqTopicSender_url(sender));
            //if available also set container name
            const char *cn = celix_bundleContext_getProperty(psa->ctx, "CELIX_CONTAINER_NAME", NULL);
            if (cn != NULL) {
                celix_properties_set(newEndpoint, "container_name", cn);
            }

            //TODO connect endpoints to sender, NOTE is this needed for a zmq topic sender?
            hashMap_put(psa->topicSenders.map, key, sender);
        } else {
            L_ERROR("[PSA ZMQ] Error creating a TopicSender");
            free(key);
        }
    } else {
        free(key);
        L_ERROR("[PSA_ZMQ] Cannot setup already existing TopicSender for scope/topic %s/%s!", scope, topic);
    }
    celixThreadMutex_unlock(&psa->topicSenders.mutex);


    if (newEndpoint != NULL && outPublisherEndpoint != NULL) {
        *outPublisherEndpoint = newEndpoint;
    }

    return status;
}

celix_status_t pubsub_zmqAdmin_teardownTopicSender(void *handle, const char *scope, const char *topic) {
    pubsub_zmq_admin_t *psa = handle;
    celix_status_t  status = CELIX_SUCCESS;

    //1) Find and remove TopicSender from map
    //2) destroy topic sender

    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    celixThreadMutex_lock(&psa->topicSenders.mutex);
    hash_map_entry_t *entry = hashMap_getEntry(psa->topicSenders.map, key);
    if (entry != NULL) {
        char *mapKey = hashMapEntry_getKey(entry);
        pubsub_zmq_topic_sender_t *sender = hashMap_remove(psa->topicSenders.map, key);
        free(mapKey);
        //TODO disconnect endpoints to sender. note is this needed for a zmq topic sender?
        pubsub_zmqTopicSender_destroy(sender);
    } else {
        L_ERROR("[PSA ZMQ] Cannot teardown TopicSender with scope/topic %s/%s. Does not exists", scope, topic);
    }
    celixThreadMutex_unlock(&psa->topicSenders.mutex);
    free(key);

    return status;
}

celix_status_t pubsub_zmqAdmin_setupTopicReceiver(void *handle, const char *scope, const char *topic, long serializerSvcId, celix_properties_t **outSubscriberEndpoint) {
    pubsub_zmq_admin_t *psa = handle;

    celix_properties_t *newEndpoint = NULL;

    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    celixThreadMutex_lock(&psa->topicReceivers.mutex);
    pubsub_updmc_topic_receiver_t *receiver = hashMap_get(psa->topicReceivers.map, key);
    if (receiver == NULL) {
        receiver = pubsub_zmqTopicReceiver_create(psa->ctx, psa->log, scope, topic, serializerSvcId);
        if (receiver != NULL) {
            const char *psaType = pubsub_zmqTopicReceiver_psaType(receiver);
            const char *serType = pubsub_zmqTopicReceiver_serializerType(receiver);
            newEndpoint = pubsubEndpoint_create(psa->fwUUID, scope, topic,
                                                PUBSUB_SUBSCRIBER_ENDPOINT_TYPE, psaType, serType, NULL);

            //if available also set container name
            const char *cn = celix_bundleContext_getProperty(psa->ctx, "CELIX_CONTAINER_NAME", NULL);
            if (cn != NULL) {
                celix_properties_set(newEndpoint, "container_name", cn);
            }

            celixThreadMutex_lock(&psa->discoveredEndpoints.mutex);
            hash_map_iterator_t iter = hashMapIterator_construct(psa->discoveredEndpoints.map);
            while (hashMapIterator_hasNext(&iter)) {
                celix_properties_t *endpoint = hashMapIterator_nextValue(&iter);
                pubsub_zmqAdmin_connectEndpointToReceiver(psa, receiver, endpoint);
            }
            celixThreadMutex_unlock(&psa->discoveredEndpoints.mutex);

            hashMap_put(psa->topicReceivers.map, key, receiver);
        } else {
            L_ERROR("[PSA ZMQ] Error creating a TopicReceiver.");
            free(key);
        }
    } else {
        free(key);
        L_ERROR("[PSA_ZMQ] Cannot setup already existing TopicReceiver for scope/topic %s/%s!", scope, topic);
    }
    celixThreadMutex_unlock(&psa->topicReceivers.mutex);


    if (newEndpoint != NULL && outSubscriberEndpoint != NULL) {
        *outSubscriberEndpoint = newEndpoint;
    }

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}

celix_status_t pubsub_zmqAdmin_teardownTopicReceiver(void *handle, const char *scope, const char *topic) {
    pubsub_zmq_admin_t *psa = handle;

    char *key = pubsubEndpoint_createScopeTopicKey(scope, topic);
    celixThreadMutex_lock(&psa->topicReceivers.mutex);
    hash_map_entry_t *entry = hashMap_getEntry(psa->topicReceivers.map, key);
    free(key);
    if (entry != NULL) {
        char *receiverKey = hashMapEntry_getKey(entry);
        pubsub_updmc_topic_receiver_t *receiver = hashMapEntry_getValue(entry);
        hashMap_remove(psa->topicReceivers.map, receiverKey);

        free(receiverKey);
        pubsub_zmqTopicReceiver_destroy(receiver);
    }
    celixThreadMutex_lock(&psa->topicReceivers.mutex);

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}

static celix_status_t pubsub_zmqAdmin_connectEndpointToReceiver(pubsub_zmq_admin_t* psa, pubsub_updmc_topic_receiver_t *receiver, const celix_properties_t *endpoint) {
    //note can be called with discoveredEndpoint.mutex lock
    celix_status_t status = CELIX_SUCCESS;

    const char *scope = pubsub_zmqTopicReceiver_scope(receiver);
    const char *topic = pubsub_zmqTopicReceiver_topic(receiver);

    const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, NULL);
    const char *eScope = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_SCOPE, NULL);
    const char *eTopic = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_NAME, NULL);
    const char *url = celix_properties_get(endpoint, PUBSUB_ZMQ_URL_KEY, NULL);

    if (type == NULL || url == NULL) {
        fprintf(stderr, "[PSA UPDMC] Error got endpoint without zmq url or endpoint type");
        status = CELIX_BUNDLE_EXCEPTION;
    } else {
        if (eScope != NULL && eTopic != NULL && type != NULL &&
            strncmp(eScope, scope, 1024 * 1024) == 0 &&
            strncmp(eTopic, topic, 1024 * 1024) == 0 &&
            strncmp(type, PUBSUB_PUBLISHER_ENDPOINT_TYPE, strlen(PUBSUB_PUBLISHER_ENDPOINT_TYPE)) == 0) {
            pubsub_zmqTopicReceiver_connectTo(receiver, url);
        }
    }

    return status;
}

celix_status_t pubsub_zmqAdmin_addEndpoint(void *handle, const celix_properties_t *endpoint) {
    pubsub_zmq_admin_t *psa = handle;
    celixThreadMutex_lock(&psa->topicReceivers.mutex);
    hash_map_iterator_t iter = hashMapIterator_construct(psa->topicReceivers.map);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_updmc_topic_receiver_t *receiver = hashMapIterator_nextValue(&iter);
        pubsub_zmqAdmin_connectEndpointToReceiver(psa, receiver, endpoint);
    }
    celixThreadMutex_unlock(&psa->topicReceivers.mutex);

    celixThreadMutex_lock(&psa->discoveredEndpoints.mutex);
    celix_properties_t *cpy = celix_properties_copy(endpoint);
    const char *uuid = celix_properties_get(cpy, PUBSUB_ENDPOINT_UUID, NULL);
    hashMap_put(psa->discoveredEndpoints.map, (void*)uuid, cpy);
    celixThreadMutex_unlock(&psa->discoveredEndpoints.mutex);

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}


static celix_status_t pubsub_zmqAdmin_disconnectEndpointFromReceiver(pubsub_zmq_admin_t* psa, pubsub_updmc_topic_receiver_t *receiver, const celix_properties_t *endpoint) {
    //note can be called with discoveredEndpoint.mutex lock
    celix_status_t status = CELIX_SUCCESS;

    const char *scope = pubsub_zmqTopicReceiver_scope(receiver);
    const char *topic = pubsub_zmqTopicReceiver_topic(receiver);

    const char *type = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TYPE, NULL);
    const char *eScope = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_SCOPE, NULL);
    const char *eTopic = celix_properties_get(endpoint, PUBSUB_ENDPOINT_TOPIC_NAME, NULL);
    const char *url = celix_properties_get(endpoint, PUBSUB_ZMQ_URL_KEY, NULL);

    if (type == NULL || url == NULL) {
        fprintf(stderr, "[PSA UPDMC] Error got endpoint without zmq url or endpoint type");
        status = CELIX_BUNDLE_EXCEPTION;
    } else {
        if (eScope != NULL && eTopic != NULL && type != NULL &&
            strncmp(eScope, scope, 1024 * 1024) == 0 &&
            strncmp(eTopic, topic, 1024 * 1024) == 0 &&
            strncmp(type, PUBSUB_PUBLISHER_ENDPOINT_TYPE, strlen(PUBSUB_PUBLISHER_ENDPOINT_TYPE)) == 0) {
            pubsub_zmqTopicReceiver_disconnectFrom(receiver, url);
        }
    }

    return status;
}

celix_status_t pubsub_zmqAdmin_removeEndpoint(void *handle, const celix_properties_t *endpoint) {
    pubsub_zmq_admin_t *psa = handle;
    celixThreadMutex_lock(&psa->topicReceivers.mutex);
    hash_map_iterator_t iter = hashMapIterator_construct(psa->topicReceivers.map);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_updmc_topic_receiver_t *receiver = hashMapIterator_nextValue(&iter);
        pubsub_zmqAdmin_disconnectEndpointFromReceiver(psa, receiver, endpoint);
    }
    celixThreadMutex_unlock(&psa->topicReceivers.mutex);

    celixThreadMutex_lock(&psa->discoveredEndpoints.mutex);
    const char *uuid = celix_properties_get(endpoint, PUBSUB_ENDPOINT_UUID, NULL);
    celix_properties_t *found = hashMap_remove(psa->discoveredEndpoints.map, (void*)uuid);
    celixThreadMutex_unlock(&psa->discoveredEndpoints.mutex);

    if (found != NULL) {
        celix_properties_destroy(found);
    }

    celix_status_t  status = CELIX_SUCCESS;
    return status;
}

celix_status_t pubsub_zmqAdmin_executeCommand(void *handle, char *commandLine __attribute__((unused)), FILE *out, FILE *errStream __attribute__((unused))) {
    pubsub_zmq_admin_t *psa = handle;
    celix_status_t  status = CELIX_SUCCESS;

    fprintf(out, "\n");
    fprintf(out, "Topic Senders:\n");
    celixThreadMutex_lock(&psa->topicSenders.mutex);
    hash_map_iterator_t iter = hashMapIterator_construct(psa->topicSenders.map);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_zmq_topic_sender_t *sender = hashMapIterator_nextValue(&iter);
        const char *psaType = pubsub_zmqTopicSender_psaType(sender);
        const char *serType = pubsub_zmqTopicSender_serializerType(sender);
        const char *scope = pubsub_zmqTopicSender_scope(sender);
        const char *topic = pubsub_zmqTopicSender_topic(sender);
        const char *url = pubsub_zmqTopicSender_url(sender);
        fprintf(out, "|- Topic Sender %s/%s\n", scope, topic);
        fprintf(out, "   |- psa type        = %s\n", psaType);
        fprintf(out, "   |- serializer type = %s\n", serType);
        fprintf(out, "   |- url             = %s\n", url);
    }
    celixThreadMutex_unlock(&psa->topicSenders.mutex);

    fprintf(out, "\n");
    fprintf(out, "\nTopic Receivers:\n");
    celixThreadMutex_lock(&psa->topicReceivers.mutex);
    iter = hashMapIterator_construct(psa->topicReceivers.map);
    while (hashMapIterator_hasNext(&iter)) {
        pubsub_updmc_topic_receiver_t *receiver = hashMapIterator_nextValue(&iter);
        const char *psaType = pubsub_zmqTopicReceiver_psaType(receiver);
        const char *serType = pubsub_zmqTopicReceiver_serializerType(receiver);
        const char *scope = pubsub_zmqTopicReceiver_scope(receiver);
        const char *topic = pubsub_zmqTopicReceiver_topic(receiver);
        fprintf(out, "|- Topic Receiver %s/%s\n", scope, topic);
        fprintf(out, "   |- psa type        = %s\n", psaType);
        fprintf(out, "   |- serializer type = %s\n", serType);
    }
    celixThreadMutex_unlock(&psa->topicSenders.mutex);
    fprintf(out, "\n");

    //TODO topic receivers/senders connection count

    return status;
}

#ifndef ANDROID
static celix_status_t zmq_getIpAddress(const char* interface, const char** ip) {
    celix_status_t status = CELIX_BUNDLE_EXCEPTION;

    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) != -1)
    {
        for (ifa = ifaddr; ifa != NULL && status != CELIX_SUCCESS; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL)
                continue;

            if ((getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0) && (ifa->ifa_addr->sa_family == AF_INET)) {
                if (interface == NULL) {
                    *ip = strdup(host);
                    status = CELIX_SUCCESS;
                }
                else if (strcmp(ifa->ifa_name, interface) == 0) {
                    *ip = strdup(host);
                    status = CELIX_SUCCESS;
                }
            }
        }

        freeifaddrs(ifaddr);
    }

    return status;
}
#endif