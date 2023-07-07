#!/usr/bin/env python3
# Copyright (c) 2014 The Bitcoin Core developers
# Copyright (c) 2018 The Zencash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
from test_framework.test_framework import BitcoinTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_greater_than, initialize_chain_clean, \
    start_nodes, sync_blocks, sync_mempools, connect_nodes_bi, mark_logs,\
    get_epoch_data, assert_false, swap_bytes
from test_framework.test_framework import ForkHeights
from test_framework.mc_test.mc_test import CertTestUtils, generate_random_field_element_hex
import time
from decimal import Decimal

DEBUG_MODE = 1
NUMB_OF_NODES = 3
EPOCH_LENGTH = 17
FT_SC_FEE = Decimal('0')
MBTR_SC_FEE = Decimal('0')
CERT_FEE = Decimal('0.00015')
GET_BLOCK_TEMPLATE_DELAY = 5 + 1 # Seconds ("+1" due to the fact the getblocktemplate trigger condition is ">5")


class sc_cert_base(BitcoinTestFramework):

    def setup_chain(self, split=False):
        print("Initializing test directory " + self.options.tmpdir)
        initialize_chain_clean(self.options.tmpdir, NUMB_OF_NODES)
        self.firstRound = True

    def setup_network(self, split=False):
        self.nodes = []

        self.nodes = start_nodes(NUMB_OF_NODES, self.options.tmpdir, extra_args=
            [['-debug=py', '-debug=sc', '-debug=mempool', '-debug=net', '-debug=cert', '-debug=zendoo_mc_cryptolib', '-scproofqueuesize=0', '-logtimemicros=1']] * NUMB_OF_NODES)

        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 1, 2)
        sync_blocks(self.nodes[1:NUMB_OF_NODES])
        sync_mempools(self.nodes[1:NUMB_OF_NODES])
        self.is_network_split = split
        self.sync_all()

    def check_certificates_ordering_by_epoch(self, block_template, first_epoch) -> bool:
        '''
        This function checks that certificates for a non-ceasable sidechain submitted to mempool
        are correctly ordered by epoch.
        '''
        for raw_certificate in block_template['certificates']:
            certificate = self.nodes[0].decoderawtransaction(raw_certificate["data"])
            assert_equal(certificate["cert"]["epochNumber"], first_epoch)
            first_epoch += 1

    def check_certificates_ordering_by_quality(self, block_template, lowest_quality):
        '''
        This function checks that certificates for a ceasable sidechain submitted to mempool
        are correctly ordered by quality.
        '''
        for raw_certificate in block_template['certificates']:
            certificate = self.nodes[0].decoderawtransaction(raw_certificate["data"])
            assert_equal(certificate["cert"]["quality"], lowest_quality)
            lowest_quality += 1

    def run_test_with_scversion(self, scversion, ceasable = True):

        '''
        The test checks that the "GetBlockTemplate" command correctly detects a new certificate in the mempool,
        in the same way as it happens for normal transactions.
        '''

        # amounts
        creation_amount = Decimal("100")
        bwt_amount = Decimal("1.0")
        sc_name = "sc" + str(scversion) + str(ceasable)

        # Adjust epoch length according to sc specifications
        epoch_length = 0 if not ceasable else EPOCH_LENGTH

        if (self.firstRound):
            mark_logs(f"Node 0 generates {ForkHeights['NON_CEASING_SC']} blocks", self.nodes, DEBUG_MODE)
            self.nodes[0].generate(ForkHeights['NON_CEASING_SC'])
            self.sync_all()
            self.firstRound = False

        #generate wCertVk and constant
        mcTest = CertTestUtils(self.options.tmpdir, self.options.srcdir)
        vk = mcTest.generate_params(sc_name, keyrot = True if scversion >= 2 else None)
        constant = generate_random_field_element_hex()
        cmdInput = {
            "version": scversion,
            "withdrawalEpochLength": epoch_length,
            "toaddress": "dada",
            "amount": creation_amount,
            "wCertVk": vk,
            "constant": constant,
        }

        ret = self.nodes[0].sc_create(cmdInput)
        scid = ret['scid']
        scid_swapped = str(swap_bytes(scid))
        mark_logs("Node 0 created a SC", self.nodes, DEBUG_MODE)

        nblocks = epoch_length if ceasable else 2
        mark_logs(f"Node 0 generating {nblocks} more blocks to confirm the sidechain and reach the end of withdrawal epoch", self.nodes, DEBUG_MODE)
        self.nodes[0].generate(nblocks)
        self.sync_all()

        epoch_number, epoch_cum_tree_hash, prev_cert_data_hash = get_epoch_data(scid, self.nodes[0], epoch_length, not ceasable)

        addr_node1 = self.nodes[1].getnewaddress()

        #Create proof for WCert
        quality = 10
        proof = mcTest.create_test_proof(sc_name,
                                         scid_swapped,
                                         epoch_number,
                                         quality,
                                         MBTR_SC_FEE,
                                         FT_SC_FEE,
                                         epoch_cum_tree_hash,
                                         prev_cert_hash = prev_cert_data_hash if scversion >= 2 else None,
                                         constant       = constant,
                                         pks            = [addr_node1],
                                         amounts        = [bwt_amount])

        amount_cert_1 = [{"address": addr_node1, "amount": bwt_amount}]

        cur_h = self.nodes[0].getblockcount()
        ret = self.nodes[0].getscinfo(scid, True, False)['items'][0]
        ceas_h = ret['ceasingHeight'] if ceasable else cur_h + 1
        ceas_limit_delta = ceas_h - cur_h - 1
        mark_logs(f"Node 0 generating {ceas_limit_delta} blocks reaching the third to last block before the SC ceasing", self.nodes, DEBUG_MODE)
        self.nodes[0].generate(ceas_limit_delta - 2)
        self.sync_all()

        cacheTime = [0, 0, 0]

        ### 1: Creating a transaction
        node2_address = self.nodes[2].getnewaddress()
        mark_logs("\nCall GetBlockTemplate on each node to create a cached (empty) version; check it is empty", self.nodes, DEBUG_MODE)
        for i in range(0, NUMB_OF_NODES):
            cacheTime[i] = time.time()
            blocktemplate = self.nodes[i].getblocktemplate()
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs("Node 0 sends a transaction", self.nodes, DEBUG_MODE)
        self.nodes[0].sendtoaddress(node2_address, 0.1)
        sync_mempools(self.nodes, 0.1) # syncing mempools only, because there is no need to check for blocks sync and here it's important to be as quick as possible

        mark_logs("Check that the transaction is not immediately included into the block template", self.nodes, DEBUG_MODE)
        for i in range(0, NUMB_OF_NODES):
            blocktemplate = self.nodes[i].getblocktemplate()
            mark_logs(f"Elapsed seconds from last cached result: {time.time() - cacheTime[i]}", self.nodes, DEBUG_MODE)
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs(f"Wait {GET_BLOCK_TEMPLATE_DELAY} seconds and check that the transaction is now included into the block template", self.nodes, DEBUG_MODE)
        time.sleep(GET_BLOCK_TEMPLATE_DELAY)
        for i in range(0, NUMB_OF_NODES):
            blocktemplate = self.nodes[i].getblocktemplate()
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 1)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs("Node 0 mines one block to clean the mempool", self.nodes, DEBUG_MODE)
        self.nodes[0].generate(1)
        self.sync_all()

        ### 2: Creating a certificate
        mark_logs("\nCall GetBlockTemplate on each node to create a new cached version; check it is empty", self.nodes, DEBUG_MODE)
        for i in range(0, NUMB_OF_NODES):
            cacheTime[i] = time.time()
            blocktemplate = self.nodes[i].getblocktemplate()
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs("Node 0 sends a certificate", self.nodes, DEBUG_MODE)
        try:
            cert_epoch_0 = self.nodes[0].sc_send_certificate(scid, epoch_number, quality,
                epoch_cum_tree_hash, proof, amount_cert_1, FT_SC_FEE, MBTR_SC_FEE, CERT_FEE)
            assert(len(cert_epoch_0) > 0)
        except JSONRPCException as e:
            errorString = e.error['message']
            mark_logs(f"Send certificate failed with reason {errorString}", self.nodes, DEBUG_MODE)
            assert(False)
        sync_mempools(self.nodes, 0.1) # syncing mempools only, because there is no need to check for blocks sync and here it's important to be as quick as possible

        mark_logs("Check that the certificate is not immediately included into the block template", self.nodes, DEBUG_MODE)
        for i in range(0, NUMB_OF_NODES):
            blocktemplate = self.nodes[i].getblocktemplate()
            mark_logs(f"Elapsed seconds from last cached result: {time.time() - cacheTime[i]}", self.nodes, DEBUG_MODE)
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs(f"Wait {GET_BLOCK_TEMPLATE_DELAY} seconds and check that the certificate is now included into the block template", self.nodes, DEBUG_MODE)
        time.sleep(GET_BLOCK_TEMPLATE_DELAY)
        for i in range(0, NUMB_OF_NODES):
            blocktemplate = self.nodes[i].getblocktemplate()
            assert_equal(len(blocktemplate['certificates']), 1)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs("Node 0 mines one block to clean the mempool", self.nodes, DEBUG_MODE)
        self.nodes[0].generate(1)
        self.sync_all()

        ### 3: Sending a transaction and a certificate
        # Generate proof before the call of `getblocktemplate`.
        # It is a time consuming operation, so may take more than GET_BLOCK_TEMPLATE_DELAY seconds.
        quality += 1
        epoch_number, epoch_cum_tree_hash, prev_cert_data_hash = get_epoch_data(scid, self.nodes[0], epoch_length, not ceasable)
        proof = mcTest.create_test_proof(
            sc_name, scid_swapped, epoch_number, quality, MBTR_SC_FEE, FT_SC_FEE, epoch_cum_tree_hash,
            prev_cert_hash = prev_cert_data_hash if scversion >= 2 else None, constant = constant, pks = [addr_node1], amounts = [bwt_amount])
        node2_address = self.nodes[2].getnewaddress()

        mark_logs("\nCall GetBlockTemplate on each node to create a new cached version; check it is empty", self.nodes, DEBUG_MODE)
        for i in range(0, NUMB_OF_NODES):
            cacheTime[i] = time.time()
            blocktemplate = self.nodes[i].getblocktemplate()
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs("Node 0 sends a transaction and a certificate", self.nodes, DEBUG_MODE)
        self.nodes[0].sendtoaddress(node2_address, 0.1, "", "", True)

        try:
            cert_epoch_0 = self.nodes[0].sc_send_certificate(scid, epoch_number, quality,
                epoch_cum_tree_hash, proof, amount_cert_1, FT_SC_FEE, MBTR_SC_FEE, CERT_FEE)
            assert(len(cert_epoch_0) > 0)
        except JSONRPCException as e:
            errorString = e.error['message']
            mark_logs(f"Send certificate failed with reason {errorString}", self.nodes, DEBUG_MODE)
            assert(False)
        sync_mempools(self.nodes, 0.1) # syncing mempools only, because there is no need to check for blocks sync and here it's important to be as quick as possible

        mark_logs("Check that the transaction and the certificate are not immediately included into the block template", self.nodes, DEBUG_MODE)
        for i in range(0, NUMB_OF_NODES):
            blocktemplate = self.nodes[i].getblocktemplate()
            mark_logs(f"Elapsed seconds from last cached result: {time.time() - cacheTime[i]}", self.nodes, DEBUG_MODE)
            assert_equal(len(blocktemplate['certificates']), 0)
            assert_equal(len(blocktemplate['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        mark_logs(f"Wait {GET_BLOCK_TEMPLATE_DELAY} seconds and check that the transaction and the certificate are now included into the block template", self.nodes, DEBUG_MODE)
        time.sleep(GET_BLOCK_TEMPLATE_DELAY)
        for i in range(0, NUMB_OF_NODES):
            blocktemplate = self.nodes[i].getblocktemplate()
            assert_equal(len(blocktemplate['certificates']), 1)
            assert_equal(len(blocktemplate['transactions']), 1)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)

        for i in range(0, NUMB_OF_NODES):
            # Check that `getblocktemplate` doesn't include "merkleTree" and "scTxsCommitment" if not explicitly requested
            gbt = self.nodes[i].getblocktemplate()
            assert_false('merkleTree' in gbt)
            assert_false('scTxsCommitment' in gbt)
            gbt = self.nodes[i].getblocktemplate({}, False)
            assert_false('merkleTree' in gbt)
            assert_false('scTxsCommitment' in gbt)

            # Check that `getblocktemplate` "merkleTree" and "scTxsCommitment" match `getblockmerkleroots`
            gbt = self.nodes[i].getblocktemplate({}, True)
            roots = self.nodes[i].getblockmerkleroots([gbt['coinbasetxn']['data']] + [x['data'] for x in gbt['transactions']], [x['data'] for x in gbt['certificates']])
            assert_equal(gbt['merkleTree'], roots['merkleTree'])
            assert_equal(gbt['scTxsCommitment'], roots['scTxsCommitment'])

        last_certificate_height = self.nodes[0].getblockcount()

        # Non ceasable sidechains can have at most 1 certificate per block, so there's no order to check
        if not ceasable:
            return

        ### 4: Check certificates ordering (by quality)

        num_certificates = 10
        from_addresses = []

        # Send coins to be used as input for the next certificate to a new address.
        # This is done to avoid generating dependencies between certificates (e.g. the second certificate
        # spending as input the change output of the first one). The alternative would be to pick UTXOs
        # manually and then use the createrawtransaction RPC command.
        for i in range(0, num_certificates):
            addr = self.nodes[0].getnewaddress()
            self.nodes[0].sendtoaddress(addr, bwt_amount + CERT_FEE)
            from_addresses.append(addr)

        self.sync_all()

        # Note that for the non-ceasing scenario the epoch_length is set to 0
        blocks_to_generate = epoch_length

        mark_logs(f"Node 0 mines {blocks_to_generate} blocks to reach the next submission window (or just have blocks to be referenced)",
                  self.nodes, DEBUG_MODE)
        self.nodes[0].generate(blocks_to_generate)
        self.sync_all()

        lowest_quality = quality + 1

        for i in range(0, num_certificates):
            epoch_number, epoch_cum_tree_hash, prev_cert_data_hash = get_epoch_data(scid, self.nodes[0], epoch_length, not ceasable)

            quality += 1

            proof = mcTest.create_test_proof(sc_name,
                                             scid_swapped,
                                             epoch_number,
                                             quality,
                                             MBTR_SC_FEE,
                                             FT_SC_FEE,
                                             epoch_cum_tree_hash,
                                             prev_cert_hash = prev_cert_data_hash if scversion >= 2 else None,
                                             constant       = constant,
                                             pks            = [addr_node1],
                                             amounts        = [bwt_amount])

            try:
                mark_logs(f"Sending certificate {i + 1} / {num_certificates}...", self.nodes, DEBUG_MODE)
                cert_epoch = self.nodes[0].sc_send_certificate(scid, epoch_number, quality, epoch_cum_tree_hash,
                                proof, amount_cert_1, FT_SC_FEE, MBTR_SC_FEE, CERT_FEE, from_addresses[i])
                assert(len(cert_epoch) > 0)
            except JSONRPCException as e:
                errorString = e.error['message']
                mark_logs(f"Send certificate failed with reason {errorString}", self.nodes, DEBUG_MODE)
                assert(False)
            self.sync_all()

        mark_logs(f"Wait {GET_BLOCK_TEMPLATE_DELAY} seconds and check that the certificates are now included into the block template", self.nodes, DEBUG_MODE)
        time.sleep(GET_BLOCK_TEMPLATE_DELAY)
        for i in range(0, NUMB_OF_NODES):
            block_template = self.nodes[i].getblocktemplate()
            assert_equal(len(block_template['certificates']), num_certificates)
            assert_equal(len(block_template['transactions']), 0)
            mark_logs(f"Node{i} ok", self.nodes, DEBUG_MODE)
            self.check_certificates_ordering_by_quality(block_template, lowest_quality)

        mark_logs("Mine one block and sync to check that the block is valid", self.nodes, DEBUG_MODE)
        # If there is any error in the order of certificates, generation of the block would fail.
        self.nodes[0].generate(1)
        self.sync_all()

    def run_test(self):
        mark_logs("\n**SC version 0", self.nodes, DEBUG_MODE)
        self.run_test_with_scversion(0)
        mark_logs("\n**SC version 2 - ceasable SC", self.nodes, DEBUG_MODE)
        self.run_test_with_scversion(2, True)
        mark_logs("\n**SC version 2 - non-ceasable SC", self.nodes, DEBUG_MODE)
        self.run_test_with_scversion(2, False)


if __name__ == '__main__':
    sc_cert_base().main()
