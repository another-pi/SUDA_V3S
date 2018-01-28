/*
 * ethercat_wrapper.c
 */

#include "ethercat_wrapper.h"
#include "slave.h"
#include "ethercat_wrapper_slave.h"

#include <ecrt.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SDO_REQUEST_TIMEOUT     500  /* ms taken from etherlab example */

#ifndef VERSIONING
#error no version information
#endif

#define XSTR(a)      STR(a)
#define STR(a)       #a

const char *g_version = "v" XSTR(VERSIONING);

#if 0 /* draft */
struct _pdo_memory {
    uint8_t *processdataa;
    uiint32_t **offset; ////< lsit of uint32_T pointer to individual pdo offsets.
};
#endif

/* -> Ethercat_Master_t */
struct _ecw_master_t {
    int id;
    /* master information */
    ec_master_t *master;
    ec_master_info_t *info;

    /* variables for data structures */
    ec_domain_t *domain;
    ec_pdo_entry_reg_t *domain_reg;
    uint8_t *processdata; /* FIXME are they needed here? */

    /* slaves */
    Ethercat_Slave_t *slave; ///<< list of slaves
    size_t slave_count;

    /* diagnostic data structures */
    ec_master_state_t master_state;
    ec_domain_state_t domain_state;
};

static FILE *g_outstream = NULL;

static void update_domain_state(Ethercat_Master_t *master);
static void update_master_state(Ethercat_Master_t *master);
static void update_all_slave_state(Ethercat_Master_t *master);
static void update_sdo_requests(Ethercat_Master_t *master);


const char *ecw_master_get_version(void)
{
    return g_version;
}

/*
 * get the pdo type according to the bit length
 *
 * FIXME support single bit PDO length
 */
enum eValueType get_type_from_bitlength(int bit_length)
{
    enum eValueType type = VALUE_TYPE_NONE;

    if (bit_length != 1 && bit_length % 2 != 0) {
        fprintf(g_outstream, "Warning mapping is either padding or wrong!\n");
        return type;
    }

    switch (bit_length) {
    case 1:
        type = VALUE_TYPE_UNSIGNED1;
        break;
    case 8:
        type = VALUE_TYPE_UNSIGNED8;
        break;
    case 16:
        type = VALUE_TYPE_UNSIGNED16;
        break;
    case 32:
        type = VALUE_TYPE_UNSIGNED32;
        break;
    default:
        type = VALUE_TYPE_NONE;
        fprintf(g_outstream, "Warning, bit size: %d not supported\n", bit_length);
        break;
    }

    return type;
}

static int setup_sdo_request(Ethercat_Slave_t *slave)
{
    for (size_t sdoindex = 0; sdoindex < slave->sdo_count; sdoindex++) {
        Sdo_t *sdo = slave->dictionary + sdoindex;

        sdo->request = ecrt_slave_config_create_sdo_request(slave->config, sdo->index, sdo->subindex, (sdo->bit_length / 8));

        if (sdo->request == NULL) {
            fprintf(g_outstream, "Warning, could not create sdo request for cyclic operation!\n");
            return -1;
        } else {
            ecrt_sdo_request_timeout(sdo->request, SDO_REQUEST_TIMEOUT);
            sdo->request_state = ecrt_sdo_request_state(sdo->request);
            sdo->read_request = 0;
        }
    }

    return 0;
}

/*
 * populate the fields:
 * master->slave[*]->[rt]xpdo
 *
 * FIXME Move to slave.c:ecw_slave_scan()
 */
static int slave_config(Ethercat_Master_t *master, int slaveindex)
{
    Ethercat_Slave_t *slave = master->slave + slaveindex;
    slave->master       = master->master;
    slave->info         = malloc(sizeof(ec_slave_info_t));
    if (ecrt_master_get_slave(master->master, slaveindex, slave->info) != 0) {
        fprintf(g_outstream, "Error, could not read slave config for slave %d\n", slaveindex);
        return -1;
    }

    slave->sdo_count   = slave->info->sdo_count;
    slave->type        = type_map_get_type(slave->info->vendor_id,
                                             slave->info->product_code);

    slave->outpdocount  = 0;
    slave->inpdocount  = 0;

    /* add one more field for the sync manager count because of the last element */
    slave->sminfo  = malloc((slave->info->sync_count + 1) * sizeof(ec_sync_info_t));

    for (int j = 0; j < slave->info->sync_count; j++) {
        ec_sync_info_t *sminfo = slave->sminfo + j;
        ecrt_master_get_sync_manager(master->master, slave->info->position, j, sminfo);

        /* if there is no PDO, this SyncManager is for mailbox communication */
        if (sminfo->n_pdos == 0)
            continue;

        if (sminfo->pdos == NULL) {
            //fprintf(g_outstream, "Warning, slave unconfigured\n");
            sminfo->pdos = malloc(sminfo->n_pdos * sizeof(ec_pdo_info_t));
        }

        for (int k = 0; k < sminfo->n_pdos; k++) {
            ec_pdo_info_t *pdoinfo = sminfo->pdos + k;
            ecrt_master_get_pdo(master->master, slave->info->position, j, k, pdoinfo);

            if (pdoinfo->entries == NULL) {
                //fprintf(g_outstream, "Warning pdoinfo.entries is NULL!\n");
                pdoinfo->entries = malloc(pdoinfo->n_entries * sizeof(ec_pdo_entry_info_t));
            }

            if (sminfo->dir == EC_DIR_OUTPUT) {
                slave->outpdocount += pdoinfo->n_entries;
                slave->output_values = calloc(slave->outpdocount, sizeof(pdo_t));
            } else if (sminfo->dir == EC_DIR_INPUT) {
                slave->inpdocount += pdoinfo->n_entries;
                slave->input_values = calloc(slave->inpdocount, sizeof(pdo_t));
            } else {
                /* FIXME error handling? */
                fprintf(g_outstream, "WARNING undefined direction\n");
            }

            for (int l = 0; l < pdoinfo->n_entries; l++) {
                ec_pdo_entry_info_t *pdoentry = pdoinfo->entries + l;
                ecrt_master_get_pdo_entry(master->master, slave->info->position, j, k, l, pdoentry);
            }
        }
    }

    /* Add the last pivot element to the list of sync managers, this is
     * necessary within the etherlab master when the PDOs are configured. */
    ec_sync_info_t *sminfo = slave->sminfo + slave->info->sync_count;
    *sminfo = (ec_sync_info_t){ 0xff };

    slave->config = ecrt_master_slave_config(master->master, slave->alias, slave->info->position, slave->info->vendor_id, slave->info->product_code);
    if (slave->config == NULL) {
        fprintf(g_outstream, "Error aquire slave configuration\n");
        return -1;
    }

    if (ecrt_slave_config_pdos(slave->config, EC_END, slave->sminfo)) {
        fprintf(g_outstream, "Error, failed to configure PDOs\n");
        return -1;
    }

    /*
     * read slave object dictionary and get the values
     */

    /* count the real number of object entries (including subindexes) */
    size_t object_count = slave->sdo_count;
    for (int i = 0; i < slave->sdo_count; i++) {
        ec_sdo_info_t sdoi;
        if (ecrt_sdo_info_get(master->master, slave->info->position, i, &sdoi)) {
            fprintf(g_outstream, "Error, unable to retrieve information of object dictionary info %d\n", i);
            return -1;
        }

        object_count += sdoi.maxindex;
    }

    slave->dictionary = malloc(/*slave->sdo_count */object_count * sizeof(Sdo_t));

    for (int i = 0, current_sdo = 0; i < slave->sdo_count; i++) {
        ec_sdo_info_t sdoi;
        //size_t current_sdo = 0;

        if (ecrt_sdo_info_get(master->master, slave->info->position, i, &sdoi)) {
            fprintf(g_outstream, "Warning, unable to retrieve information of object dictionary entry %d\n", i);
            continue;
        }

        for (int j = 0; j < (sdoi.maxindex + 1); j++) {
            Sdo_t *sdo = slave->dictionary + current_sdo++;
            ec_sdo_info_entry_t entry;

            if (ecrt_sdo_get_info_entry(master->master, slave->info->position, sdoi.index, j, &entry)) {
                fprintf(g_outstream, "Warning, cannot read SDO entry index: 0x%04x subindex: %d\n", sdoi.index, j);
                continue;
            }

            sdo->index      = sdoi.index;
            sdo->subindex   = j;
            sdo->entry_type = entry.data_type;
            sdo->object_type = sdoi.object_code;
            sdo->bit_length = entry.bit_length;
            sdo->value      = 0;

            memmove(sdo->name, entry.description, EC_MAX_STRING_LENGTH);
            memmove(sdo->read_access, entry.read_access, EC_SDO_ENTRY_ACCESS_COUNTER);
            memmove(sdo->write_access, entry.write_access, EC_SDO_ENTRY_ACCESS_COUNTER);

            /* SDO requests are uploaded at master_start(), they are only
             * needed when master and slave are in reql time context. */
            sdo->request = NULL;
        }
    }

    slave->sdo_count = object_count;

    return 0;
}

void ecw_print_topology(Ethercat_Master_t *master)
{
    ec_slave_info_t *slaveinfo;

    for (int i = 0; i < master->info->slave_count; i++) {
        (master->slave + i)->info = malloc(sizeof(ec_slave_info_t));
        slaveinfo = (master->slave + i)->info;

        if (ecrt_master_get_slave(master->master, i, slaveinfo) != 0) {
            fprintf(g_outstream, "[DEBUG %s] Couldn't read slave config on position %d\n",
                        __func__, i);
        }

        printf("[DEBUG] slave count: %d ::\n", i);
        printf("        Position: %d\n", slaveinfo->position);
        printf("        Vendor ID: 0x%08x\n", slaveinfo->vendor_id);
        printf("        Number of SDOs: %d\n", slaveinfo->sdo_count);

        Ethercat_Slave_t *slave = master->slave + i;

        printf("\nDEBUG Output\n-------------\n");
        printf("Slave index: %d\n", slave->info->position);
        printf("      type:  %s\n", ecw_slave_type_string(slave->type));
        printf("  # Syncmanager: %d\n", slave->info->sync_count);

        printf("  out pdo count: %lu\n", slave->outpdocount);
        printf("  in  pdo count: %lu\n", slave->inpdocount);

        for (int i = 0; i < slave->info->sync_count; i++) {
            ec_sync_info_t *sminfo = slave->sminfo + i;

            printf("| Slave: %d, Syncmanager: %d\n", slave->info->position, i);
            printf("|    index: 0x%04x\n", sminfo->index);
            printf("|    direction: %d\n", sminfo->dir);
            printf("|    # of PDOs: %d\n", sminfo->n_pdos);

            if (sminfo->n_pdos == 0) {
                fprintf(stdout, "[INFO] no pdos to assaign... continue \n");
                continue;
            }

            for (int j = 0; j < sminfo->n_pdos; j++) {
                ec_pdo_info_t *pdoinfo = sminfo->pdos + j;
                if (pdoinfo == NULL) {
                    fprintf(g_outstream, "ERROR pdoinfo is not available\n");
                }

                printf("|    | PDO Info (%d):\n", j);
                printf("|    | PDO Index: 0x%04x;\n", pdoinfo->index);
                printf("|    | # of Entries: %d\n",  pdoinfo->n_entries);

                for (int k = 0; k < pdoinfo->n_entries; k++) {
                    ec_pdo_entry_info_t *entry = pdoinfo->entries + k;

                    printf("|    |   | Entry %d: 0x%04x:%d (%d)\n",
                             k, entry->index, entry->subindex, entry->bit_length);
                }
            }
        }
    }
}

void ecw_print_domainregs(Ethercat_Master_t *master)
{
    ec_pdo_entry_reg_t *domain_reg_cur = master->domain_reg;

    printf("Domain Registrations:\n");
    while (domain_reg_cur->vendor_id != 0) {
        printf("  { %d, %d, 0x%04x, 0x%04x, 0x%02x, %d, 0x%x, 0x%x  },\n",
            domain_reg_cur->alias, domain_reg_cur->position,
            domain_reg_cur->vendor_id, domain_reg_cur->product_code,
            domain_reg_cur->index, domain_reg_cur->subindex,
            *(domain_reg_cur->offset), *(domain_reg_cur->bit_position));

        domain_reg_cur++;
    }
}

void ecw_print_allslave_od(Ethercat_Master_t *master)
{
    Ethercat_Slave_t *slave = NULL;
    Sdo_t *sdo = NULL;

    for (int k = 0; k < master->slave_count; k++) {
        slave = master->slave + k;
        printf("[DEBUG] Slave %d, number of SDOs: %lu\n", slave->info->position, ecw_slave_get_sdo_count(slave));

        for (int i = 0; i < slave->sdo_count; i++) {
            sdo = slave->dictionary + i;

            printf("    +-> Object Numder: %d ", i);
            printf(", 0x%04x:%d", sdo->index, sdo->subindex);
            printf(", %d, %d", sdo->value, sdo->bit_length);
            printf(", %d", sdo->object_type);
            printf(", %d", sdo->entry_type);
            printf(", \"%s\"\n", sdo->name);
        }
    }
}

Ethercat_Master_t *ecw_master_init(int master_id, FILE *logfile)
{
    if (logfile == NULL) {
        g_outstream = stderr;
    } else {
        g_outstream = logfile;
    }

    Ethercat_Master_t *master = malloc(sizeof(Ethercat_Master_t));
    if (master == NULL) {
        /* Cannot allocate master */
        free(master);
        return NULL;
    }

    master->master = ecrt_request_master(master_id);
    if (master->master == NULL) {
        return NULL;
    }

    /* configure slaves */
    master->info = calloc(1, sizeof(ec_master_info_t));

    int timeout = 1000;
    master->info->scan_busy = 1;
    while ((master->info->scan_busy) && (timeout-- > 0)) {
        ecrt_master(master->master, master->info);
        usleep(1000);
    }

    if (timeout <= 0) {
        fprintf(g_outstream, "ERROR, scan_busy timed out\n");
        return NULL;
    }

    master->slave_count = master->info->slave_count;
    master->slave = malloc(master->slave_count * sizeof(Ethercat_Slave_t));


    size_t all_pdo_count = 0;

    for (int i = 0; i < master->info->slave_count; i++) {
        /* get the PDOs from the buffered syncmanagers */
        slave_config(master, i);

        all_pdo_count += ((master->slave + i)->outpdocount + (master->slave + i)->inpdocount);
    }

    /*
     * Register domain for PDO exchange
     */
    master->domain_reg = malloc((all_pdo_count + 1) * sizeof(ec_pdo_entry_reg_t));
    ec_pdo_entry_reg_t *domain_reg_cur = master->domain_reg;

    for (int i = 0; i < master->slave_count; i++) {
        Ethercat_Slave_t *slave = master->slave + i;
        for (int j = 0; j < slave->info->sync_count; j++) {
            ec_sync_info_t *sm = slave->sminfo + j;
            if (0 == sm->n_pdos) { /* if no pdos for this syncmanager proceed to the next one */
                continue;
            }

            pdo_t *values = NULL;

            if (sm->dir == EC_DIR_OUTPUT) {
                values = slave->output_values;
            } else if (sm->dir == EC_DIR_INPUT) {
                values = slave->input_values;
            } else {
                fprintf(g_outstream, "ERROR could not determine direction\n");
                continue; /* skip this configuration - FIXME better error handling? */
            }

            size_t valcount = 0;

            for (int m = 0; m < sm->n_pdos; m++) {
                ec_pdo_info_t *pdos = sm->pdos + m;

                for (int n = 0; n < pdos->n_entries; n++) {
                    ec_pdo_entry_info_t *entry = pdos->entries + n;

                    pdo_t *pdoe = (values + valcount);
                    valcount++;

                    pdoe->type = get_type_from_bitlength(entry->bit_length);
                    /* FIXME Add proper error handling if VALUE_TYPE_NONE is returnd */

                    domain_reg_cur->alias        = slave->info->alias;
                    domain_reg_cur->position     = slave->info->position;
                    domain_reg_cur->vendor_id    = slave->info->vendor_id;
                    domain_reg_cur->product_code = slave->info->product_code;
                    domain_reg_cur->index        = entry->index;
                    domain_reg_cur->subindex     = entry->subindex;
                    domain_reg_cur->offset       = &(pdoe->offset);
                    domain_reg_cur->bit_position = &(pdoe->bit_offset);
                    domain_reg_cur++;
                }
            }
        }
    }
    domain_reg_cur = (ec_pdo_entry_reg_t *){ 0 };

    update_master_state(master);
    update_all_slave_state(master);

    return master;
}

void ecw_master_release(Ethercat_Master_t *master)
{
    /* for each slave */
    free(master->domain);
    free(master->slave); /* FIXME have to recursivly clean up this slave! */
    free(master->info);
    ecrt_release_master(master->master);
    free(master);
}

int ecw_master_start(Ethercat_Master_t *master)
{
    if (master->master == NULL) {
        fprintf(g_outstream, "Error, master not configured!\n");
        return -1;
    }

    /* Slave configuration for the master */
    for (size_t slaveid = 0; slaveid < master->slave_count; slaveid++) {
        Ethercat_Slave_t *slave = master->slave + slaveid;
        slave->config = ecrt_master_slave_config(master->master, slave->alias, slave->info->position, slave->info->vendor_id, slave->info->product_code);
        if (slave->config == NULL) {
            fprintf(g_outstream, "Error slave (id: %lu) configuration failed.\n", slaveid);
            return -1;
        }

        if (setup_sdo_request(slave)) {
            fprintf(g_outstream, "Error could not setup SDO requests for slave id %lu\n", slaveid);
            return -1;
        }
    }

    /* create the domain registry */
    master->domain = ecrt_master_create_domain(master->master);
    if (master->domain == NULL) {
        return -1;
    }

    if (ecrt_domain_reg_pdo_entry_list(master->domain, master->domain_reg) != 0) {
        fprintf(g_outstream, "Error cannot register PDO domain\n");
        return -1;
    }

    /* FIXME how can I get information about the error leading to no activation
     * of the master */
    if (ecrt_master_activate(master->master) < 0) {
        fprintf(g_outstream, "Error could not activate master.\n");
        return -1;
    }

    master->processdata = ecrt_domain_data(master->domain);
    if (master->processdata == NULL) {
        fprintf(g_outstream, "Error unable to get processdata pointer. Disable master again.\n");
        ecrt_master_deactivate(master->master);
        return -1;
    }

    update_domain_state(master);

    return 0;
}

int ecw_master_stop(Ethercat_Master_t *master)
{
    /* FIXME Check if master is running */

    /* These pointer will become invalid after call to ecrt_master_deactivate() */
    master->domain = NULL;
    master->processdata = NULL;

    /* The documentation of this function in ecrt.h is kind of misleading. It
     * states that this function shouldn't be called in real-time context. On the
     * otherhand the offical IgH documentation states this function as counterpart
     * to ecrt_master_activate(). */
    ecrt_master_deactivate(master->master);

    /* This function frees the following data structures (internally):
     *
     * Removes the bus configuration. All objects created by
     * ecrt_master_create_domain(), ecrt_master_slave_config(), ecrt_domain_data()
     * ecrt_slave_config_create_sdo_request() and
     * ecrt_slave_config_create_voe_handler() are freed, so pointers to them
     * become invalid.
     */

    return 0;
}


/**
 * @return number of found slaves; <0 on error
 */
int ecw_master_scan(Ethercat_Master_t *master)
{
    return 0;
}

int ecw_master_start_cyclic(Ethercat_Master_t *master)
{
    if (ecrt_master_activate(master->master)) {
        fprintf(g_outstream, "[ERROR %s] Unable ot activate master\n", __func__);
        return -1;
    }

    if (!(master->processdata = ecrt_domain_data(master->domain))) {
        fprintf(g_outstream, "[ERROR %s] Cannot access process data space\n", __func__);
        return -1;
    }

    return 0;
}

int ecw_master_stop_cyclic(Ethercat_Master_t *master)
{
    /* ecrt_master_deactivate() cleans up everything that was used for
     * the master application, during this process the pointers to the
     * generated structures become invalid. */
    master->processdata = NULL;
    master->domain = NULL;
    ecrt_master_deactivate(master->master);

    return 0;
}

/* TODO
 * this function provides the data exchange with the kernel module and has
 * to be called cyclically.
 */
int ecw_master_cyclic_function(Ethercat_Master_t *master)
{
    ecw_master_receive_pdo(master);

    update_domain_state(master);
    update_master_state(master);
    update_all_slave_state(master);

    update_sdo_requests(master);

    ecw_master_send_pdo(master);

    return 0;
}

/*
 * Handler for cyclic tasks
 */

int ecw_master_pdo_exchange(Ethercat_Master_t *master)
{
    int ret = 0;

    /* receive has to be in front of send, FIXME may merge thse two functions */
    ret = ecw_master_receive_pdo(master);
    ret = ecw_master_send_pdo(master);

    return ret;
}

int ecw_master_receive_pdo(Ethercat_Master_t *master)
{
    /* FIXME: Check error state and may add handler for broken topology. */
    ecrt_master_receive(master->master);
    ecrt_domain_process(master->domain);

    for (int i = 0; i < master->slave_count; i++) {
        Ethercat_Slave_t *slave = master->slave + i;

        for (int k = 0; k < slave->inpdocount; k++) {
            pdo_t *pdo = ecw_slave_get_inpdo(slave, k);

            switch (pdo->type) {
            case VALUE_TYPE_UNSIGNED1:
                pdo->value = EC_READ_BIT(master->processdata  + pdo->offset, pdo->bit_offset);
                break;
            case VALUE_TYPE_UNSIGNED8:
                pdo->value = EC_READ_U8(master->processdata  + pdo->offset);
                break;
            case VALUE_TYPE_UNSIGNED16:
                pdo->value = EC_READ_U16(master->processdata + pdo->offset);
                break;
            case VALUE_TYPE_UNSIGNED32:
                pdo->value = EC_READ_U32(master->processdata + pdo->offset);
                break;
            case VALUE_TYPE_SIGNED8:
                pdo->value = EC_READ_S8(master->processdata  + pdo->offset);
                break;
            case VALUE_TYPE_SIGNED16:
                pdo->value = EC_READ_S16(master->processdata + pdo->offset);
                break;
            case VALUE_TYPE_SIGNED32:
                pdo->value = EC_READ_S32(master->processdata + pdo->offset);
                break;

            case VALUE_TYPE_PADDING:
            case VALUE_TYPE_NONE:
            default:
                //fprintf(g_outstream, "Warning, unknown value type(%d). No RxPDO update\n", pdo->type);
                pdo->value = 0;
                break;
            }

            ecw_slave_set_inpdo(slave, k, pdo);
        }
    }

    return 0;
}

int ecw_master_send_pdo(Ethercat_Master_t *master)
{
    for (int i = 0; i < master->slave_count; i++) {
        Ethercat_Slave_t *slave = master->slave + i;

        for (int k = 0; k < slave->outpdocount; k++) {
            pdo_t *value = ecw_slave_get_outpdo(slave, k);

            //EC_WRITE_XX(master->processdata + (slave->txpdo_offset + k), value);
            switch (value->type) {
            case VALUE_TYPE_UNSIGNED1:
                EC_WRITE_BIT(master->processdata + value->offset, value->bit_offset, value->value);
                break;
            case VALUE_TYPE_UNSIGNED8:
                EC_WRITE_U8(master->processdata + value->offset, value->value);
                break;
            case VALUE_TYPE_UNSIGNED16:
                EC_WRITE_U16(master->processdata + value->offset, value->value);
                break;
            case VALUE_TYPE_UNSIGNED32:
                EC_WRITE_U32(master->processdata + value->offset, value->value);
                break;
            case VALUE_TYPE_SIGNED8:
                EC_WRITE_S8(master->processdata + value->offset, value->value);
                break;
            case VALUE_TYPE_SIGNED16:
                EC_WRITE_S16(master->processdata + value->offset, value->value);
                break;
            case VALUE_TYPE_SIGNED32:
                EC_WRITE_S32(master->processdata + value->offset, value->value);
                break;

            case VALUE_TYPE_PADDING:
            case VALUE_TYPE_NONE:
            default:
                //fprintf(g_outstream, "Warning, unknown value type(%d). No TxPDO update\n", value->type);
                break;
            }
        }
    }

    ecrt_domain_queue(master->domain);
    ecrt_master_send(master->master);

    return 0;
}

/*
 * Slave Handling
 */

size_t ecw_master_slave_count(Ethercat_Master_t *master)
{
    return master->slave_count;
}

size_t ecw_master_slave_responding(Ethercat_Master_t *master)
{
    return master->master_state.slaves_responding;
}

Ethercat_Slave_t *ecw_slave_get(Ethercat_Master_t *master, int slaveid)
{
    if (slaveid <= 0 && slaveid > master->slave_count)
        return NULL;

    return (master->slave + slaveid);
}

int ecw_slave_set_state(Ethercat_Master_t *master, int slaveid, enum eALState state)
{
    uint8_t int_state = ALSTATE_INIT;
    switch (state) {
    case ALSTATE_INIT:  int_state = 1; break;
    case ALSTATE_PREOP:  int_state = 2; break;
    case ALSTATE_BOOT:  int_state = 3; break;
    case ALSTATE_SAFEOP:  int_state = 4; break;
    case ALSTATE_OP:  int_state = 8; break;
    }

    return ecrt_master_slave_link_state_request(master->master, slaveid, state);
}

/*
 * State updatei functions
 */

static void update_domain_state(Ethercat_Master_t *master)
{
    ec_domain_state_t ds;
    ecrt_domain_state(master->domain, &ds/*&(master->domain_state)*/);

#if 0 /* for now disable this vast amount of prints FIXME have to figure out why this happend */
    if (ds.working_counter != master->domain_state.working_counter)
        fprintf(g_outstream, "Working counter differ: %d / %d\n",
                ds.working_counter, master->domain_state.working_counter);

    if (ds.wc_state != master->domain_state.wc_state)
        fprintf(g_outstream, "New WC State: %d / %d\n",
                ds.wc_state, master->domain_state.wc_state);
#endif

    master->domain_state = ds;
}

static void update_master_state(Ethercat_Master_t *master)
{
    ecrt_master_state(master->master, &(master->master_state));

    if (master->slave_count != master->master_state.slaves_responding) {
        fprintf(g_outstream, "Warning slaves responding: %u expected: %zu\n",
                master->master_state.slaves_responding, master->slave_count);
    }
}

static void update_all_slave_state(Ethercat_Master_t *master)
{
    for (int i = 0; i < master->slave_count; i++) {
        Ethercat_Slave_t *slave = master->slave + i;
        ecrt_slave_config_state(slave->config, &(slave->state));
    }
}

static void update_sdo_requests(Ethercat_Master_t *master)
{
    for (int i = 0; i < master->slave_count; i++) {
        Ethercat_Slave_t *slave = master->slave + i;
        for (int j = 0; j < slave->sdo_count; j++) {
            Sdo_t *sdo = slave->dictionary + j;
            sdo->request_state = ecrt_sdo_request_state(sdo->request);
            /* I have to update the value here, because the scheduled sdo
             * request can not be finisehd wihtin the normal operation. */
            if (sdo->read_request) {
                sdo->read_request = 0;
                sdo_read_value(sdo);
            }
        }
    }
}
