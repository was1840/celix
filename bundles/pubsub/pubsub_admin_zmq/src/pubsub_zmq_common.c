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

#include "pubsub_zmq_common.h"

int psa_zmq_localMsgTypeIdForMsgType(void* handle __attribute__((unused)), const char* msgType, unsigned int* msgTypeId) {
    *msgTypeId = utils_stringHash(msgType);
    return 0;
}

bool psa_zmq_checkVersion(version_pt msgVersion, pubsub_msg_header_t *hdr) {
    bool check=false;
    int major=0,minor=0;

    if(msgVersion!=NULL){
        version_getMajor(msgVersion,&major);
        version_getMinor(msgVersion,&minor);
        if(hdr->major==((unsigned char)major)){ /* Different major means incompatible */
            check = (hdr->minor>=((unsigned char)minor)); /* Compatible only if the provider has a minor equals or greater (means compatible update) */
        }
    }

    return check;
}