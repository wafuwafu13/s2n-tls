/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>

#include "api/s2n.h"
#include "crypto/s2n_fips.h"
#include "s2n_test.h"
#include "testlib/s2n_testlib.h"
#include "utils/s2n_safety.h"

#define NUM_TIED_CERTS 100

struct s2n_connection *create_conn(s2n_mode mode, struct s2n_config *config)
{
    struct s2n_connection *conn = s2n_connection_new(mode);
    PTR_GUARD_POSIX(s2n_connection_set_config(conn, config));
    return conn;
}

static int num_times_cb_executed = 0;
static struct s2n_cert_chain_and_key *test_cert_tiebreak_cb(struct s2n_cert_chain_and_key *cert1,
        struct s2n_cert_chain_and_key *cert2,
        uint8_t *name,
        uint32_t name_len)
{
    const int priority1 = *((int *) s2n_cert_chain_and_key_get_ctx(cert1));
    const int priority2 = *((int *) s2n_cert_chain_and_key_get_ctx(cert2));
    num_times_cb_executed++;
    return (priority1 > priority2 ? cert1 : cert2);
}

int main(int argc, char **argv)
{
    struct s2n_config *server_config = NULL;
    struct s2n_config *client_config = NULL;
    struct s2n_connection *server_conn = NULL;
    struct s2n_connection *client_conn = NULL;
    char *alligator_cert = NULL;
    char *alligator_key = NULL;
    char *cert_chain = NULL;
    char *private_key = NULL;

    BEGIN_TEST();
    EXPECT_SUCCESS(s2n_disable_tls13_in_test());

    struct s2n_test_io_pair io_pair;
    EXPECT_SUCCESS(s2n_io_pair_init_non_blocking(&io_pair));

    EXPECT_NOT_NULL(alligator_cert = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(alligator_key = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_ALLIGATOR_SAN_CERT, alligator_cert, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_ALLIGATOR_SAN_KEY, alligator_key, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(cert_chain = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_NOT_NULL(private_key = malloc(S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_CERT_CHAIN, cert_chain, S2N_MAX_TEST_PEM_SIZE));
    EXPECT_SUCCESS(s2n_read_test_pem(S2N_DEFAULT_TEST_PRIVATE_KEY, private_key, S2N_MAX_TEST_PEM_SIZE));

    EXPECT_SUCCESS(setenv("S2N_DONT_MLOCK", "1", 0));

    EXPECT_NOT_NULL(client_config = s2n_config_new());
    EXPECT_SUCCESS(s2n_config_disable_x509_verification(client_config));
    /* Create config with s2n_config_add_cert_chain_and_key_to_store API with multiple certs */
    {
        struct s2n_cert_chain_and_key *default_cert = NULL;
        /* Associated data to attach to each certificate to use in the tiebreak callback. */
        int tiebreak_priorites[NUM_TIED_CERTS] = { 0 };
        /* Collection of certs with the same domain name that need to have ties resolved. */
        struct s2n_cert_chain_and_key *tied_certs[NUM_TIED_CERTS] = { NULL };
        EXPECT_NOT_NULL(server_config = s2n_config_new());
        EXPECT_SUCCESS(s2n_config_set_cert_tiebreak_callback(server_config, test_cert_tiebreak_cb));

        /* Need to add at least one cert with a different domain name to make cert lookup utilize hashmap */
        EXPECT_NOT_NULL(default_cert = s2n_cert_chain_and_key_new());
        EXPECT_SUCCESS(s2n_cert_chain_and_key_load_pem(default_cert, cert_chain, private_key));
        EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(server_config, default_cert));

        /* Add NUM_TIED_CERTS that are actually the same certificate(www.alligator.com) to trigger the tiebreak callback. */
        for (unsigned int i = 0; i < NUM_TIED_CERTS; i++) {
            EXPECT_NOT_NULL(tied_certs[i] = s2n_cert_chain_and_key_new());
            EXPECT_SUCCESS(s2n_cert_chain_and_key_load_pem(tied_certs[i], alligator_cert, alligator_key));
            tiebreak_priorites[i] = i;
            EXPECT_SUCCESS(s2n_cert_chain_and_key_set_ctx(tied_certs[i], (void *) &tiebreak_priorites[i]));
            EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(server_config, tied_certs[i]));
        }

        EXPECT_NOT_NULL(server_conn = create_conn(S2N_SERVER, server_config));
        EXPECT_NOT_NULL(client_conn = create_conn(S2N_CLIENT, client_config));
        EXPECT_SUCCESS(s2n_connections_set_io_pair(client_conn, server_conn, &io_pair));
        EXPECT_SUCCESS(s2n_set_server_name(client_conn, "www.alligator.com"));
        EXPECT_SUCCESS(s2n_negotiate_test_server_and_client(server_conn, client_conn));
        EXPECT_TRUE(IS_FULL_HANDSHAKE(server_conn));
        EXPECT_EQUAL(num_times_cb_executed, NUM_TIED_CERTS - 1);
        struct s2n_cert_chain_and_key *selected_cert = s2n_connection_get_selected_cert(server_conn);
        /* The last alligator certificate should have the highest priority */
        EXPECT_EQUAL(selected_cert, tied_certs[(NUM_TIED_CERTS - 1)]);
        EXPECT_EQUAL(s2n_cert_chain_and_key_get_ctx(selected_cert), (void *) &tiebreak_priorites[(NUM_TIED_CERTS - 1)]);
        EXPECT_EQUAL(*((int *) s2n_cert_chain_and_key_get_ctx(selected_cert)), NUM_TIED_CERTS - 1);
        EXPECT_SUCCESS(s2n_shutdown_test_server_and_client(server_conn, client_conn));

        EXPECT_SUCCESS(s2n_connection_free(server_conn));
        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        for (size_t i = 0; i < NUM_TIED_CERTS; i++) {
            EXPECT_SUCCESS(s2n_cert_chain_and_key_free(tied_certs[i]));
        }
        EXPECT_SUCCESS(s2n_cert_chain_and_key_free(default_cert));
        EXPECT_SUCCESS(s2n_config_free(server_config));
    };

    /* Create config with deprecated s2n_config_add_cert_chain_and_key API */
    {
        EXPECT_NOT_NULL(server_config = s2n_config_new());
        EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key(server_config, cert_chain, private_key));

        EXPECT_NOT_NULL(server_conn = create_conn(S2N_SERVER, server_config));
        EXPECT_NOT_NULL(client_conn = create_conn(S2N_CLIENT, client_config));
        EXPECT_SUCCESS(s2n_connections_set_io_pair(client_conn, server_conn, &io_pair));

        EXPECT_SUCCESS(s2n_negotiate_test_server_and_client(server_conn, client_conn));
        EXPECT_TRUE(IS_FULL_HANDSHAKE(server_conn));
        EXPECT_SUCCESS(s2n_shutdown_test_server_and_client(server_conn, client_conn));

        EXPECT_SUCCESS(s2n_connection_free(server_conn));
        EXPECT_SUCCESS(s2n_connection_free(client_conn));
        EXPECT_SUCCESS(s2n_config_free(server_config));
    };

    /* Do not allow configs to call both
     * s2n_config_add_cert_chain_and_key and s2n_config_add_cert_chain_and_key_to_store */
    {
        DEFER_CLEANUP(struct s2n_cert_chain_and_key *chain = NULL,
                s2n_cert_chain_and_key_ptr_free);
        EXPECT_SUCCESS(s2n_test_cert_chain_and_key_new(&chain,
                S2N_DEFAULT_TEST_CERT_CHAIN, S2N_DEFAULT_TEST_PRIVATE_KEY));

        /* Config first uses s2n_config_add_cert_chain_and_key: library owns chain */
        {
            DEFER_CLEANUP(struct s2n_config *config = s2n_config_new(), s2n_config_ptr_free);
            EXPECT_NOT_NULL(config);
            EXPECT_EQUAL(config->cert_ownership, S2N_NOT_OWNED);

            /* Add first chain */
            EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key(config, cert_chain, private_key));
            EXPECT_EQUAL(config->cert_ownership, S2N_LIB_OWNED);

            /* Try to add second chain of same type */
            EXPECT_FAILURE_WITH_ERRNO(s2n_config_add_cert_chain_and_key(config, cert_chain, private_key),
                    S2N_ERR_MULTIPLE_DEFAULT_CERTIFICATES_PER_AUTH_TYPE);
            EXPECT_EQUAL(config->cert_ownership, S2N_LIB_OWNED);

            /* Try to add chain using other method */
            EXPECT_FAILURE_WITH_ERRNO(s2n_config_add_cert_chain_and_key_to_store(config, chain),
                    S2N_ERR_CERT_OWNERSHIP);
            EXPECT_EQUAL(config->cert_ownership, S2N_LIB_OWNED);
        };

        /* Config first uses s2n_config_add_cert_chain_and_key_to_store: application owns chain */
        {
            DEFER_CLEANUP(struct s2n_config *config = s2n_config_new(), s2n_config_ptr_free);
            EXPECT_NOT_NULL(config);
            EXPECT_EQUAL(config->cert_ownership, S2N_NOT_OWNED);

            /* Add first chain */
            EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(config, chain));
            EXPECT_EQUAL(config->cert_ownership, S2N_APP_OWNED);

            /* Add second chain */
            EXPECT_SUCCESS(s2n_config_add_cert_chain_and_key_to_store(config, chain));
            EXPECT_EQUAL(config->cert_ownership, S2N_APP_OWNED);

            /* Try to add chain using other method */
            EXPECT_FAILURE_WITH_ERRNO(s2n_config_add_cert_chain_and_key(config, cert_chain, private_key),
                    S2N_ERR_CERT_OWNERSHIP);
            EXPECT_EQUAL(config->cert_ownership, S2N_APP_OWNED);
        };
    };

    /* s2n_cert_chain_and_key_load_pem */
    {
        /* when loading a chain, all certs have a info associated with them and root is self-signed */
        {
            DEFER_CLEANUP(struct s2n_cert_chain_and_key *chain = NULL,
                    s2n_cert_chain_and_key_ptr_free);
            EXPECT_SUCCESS(s2n_test_cert_permutation_load_server_chain(&chain, "ec", "ecdsa",
                    "p384", "sha256"));
            struct s2n_cert *leaf = chain->cert_chain->head;
            EXPECT_EQUAL(leaf->info.self_signed, false);
            EXPECT_EQUAL(leaf->info.signature_nid, NID_ecdsa_with_SHA256);
            EXPECT_EQUAL(leaf->info.signature_digest_nid, NID_sha256);

            struct s2n_cert *intermediate = leaf->next;
            EXPECT_NOT_NULL(intermediate);
            EXPECT_EQUAL(intermediate->info.self_signed, false);
            EXPECT_EQUAL(intermediate->info.signature_nid, NID_ecdsa_with_SHA256);
            EXPECT_EQUAL(intermediate->info.signature_digest_nid, NID_sha256);

            struct s2n_cert *root = intermediate->next;
            EXPECT_NOT_NULL(intermediate);
            EXPECT_NULL(root->next);
            EXPECT_EQUAL(root->info.self_signed, true);
            EXPECT_EQUAL(root->info.signature_nid, NID_ecdsa_with_SHA256);
            EXPECT_EQUAL(root->info.signature_digest_nid, NID_sha256);
        };
    };

    EXPECT_SUCCESS(s2n_io_pair_close(&io_pair));
    EXPECT_SUCCESS(s2n_config_free(client_config));

    free(cert_chain);
    free(private_key);
    free(alligator_cert);
    free(alligator_key);
    END_TEST();
}
