---
title: Prerequisites - Replicated
headerTitle: Prerequisites for YBA
linkTitle: YBA prerequisites
description: Prerequisites for installing YugabyteDB Anywhere using Replicated.
menu:
  preview_yugabyte-platform:
    identifier: prerequisites
    parent: install-yugabyte-platform
    weight: 20
type: docs
---

There are three methods of installing YugabyteDB Anywhere (YBA):

| Method | Using | Use If |
| :--- | :--- | :--- |
| Replicated | Docker containers | You're able to use Docker containers. |
| Kubernetes | Helm chart | You're deploying in Kubernetes. |
| YBA Installer | yba-ctl CLI | You can't use Docker containers.<br/>(Note: in Early Access, contact {{% support-platform %}}) |

All installation methods support installing YBA with and without (airgapped) Internet connectivity.

Licensing (such as a license file in the case of Replicated, or appropriate repository access in the case of Kubernetes) may be required prior to installation.  Contact {{% support-platform %}} for assistance.

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="../default/" class="nav-link active">
      <i class="fa-solid fa-cloud"></i>Replicated</a>
  </li>

  <li>
    <a href="../kubernetes/" class="nav-link">
      <i class="fa-regular fa-dharmachakra" aria-hidden="true"></i>Kubernetes</a>
  </li>

  <li>
    <a href="../installer/" class="nav-link">
      <i class="fa-solid fa-building" aria-hidden="true"></i>YBA Installer</a>
  </li>

</ul>

## Supported Linux distributions

You can install YugabyteDB Anywhere on the following Linux distributions:

- CentOS (default)
- Ubuntu 18 and 20
- Other [operating systems supported by Replicated](https://www.replicated.com/docs/distributing-an-application/supported-operating-systems/).

## Hardware requirements

A node running YugabyteDB Anywhere is expected to meet the following requirements:

- 4 cores
- 8 GB memory
- 200 GB disk space

## Prepare the host

YugabyteDB Anywhere uses [Replicated scheduler](https://www.replicated.com/) for software distribution and container management. You need to ensure that the host can pull containers from the [Replicated Docker Registries](https://help.replicated.com/docs/native/getting-started/docker-registries/).

Replicated installs a compatible Docker version if it is not pre-installed on the host. The currently supported Docker version is 20.10.n.

Installing on airgapped hosts requires additional configurations, as described in [Airgapped hosts](#airgapped-hosts).

### Airgapped hosts

Installing YugabyteDB Anywhere on airgapped hosts, without access to any Internet traffic (inbound or outbound) requires the following:

- Whitelisting endpoints: To install Replicated and YugabyteDB Anywhere on a host with no Internet connectivity, you have to first download the binaries on a computer that has Internet connectivity, and then copy the files over to the appropriate host. In case of restricted connectivity, the following endpoints have to be whitelisted to ensure that they are accessible from the host marked for installation:
  `https://downloads.yugabyte.com`
  `https://download.docker.com`

- Ensuring that Docker Engine version 20.10.n is available. If it is not installed, you need to follow the procedure described in [Installing Docker in airgapped](https://www.replicated.com/docs/kb/supporting-your-customers/installing-docker-in-airgapped/).
- Ensuring that the following ports are open on the YugabyteDB Anywhere host:
  - `8800` – HTTP access to the Replicated UI
  - `80` – HTTP access to the YugabyteDB Anywhere UI
  - `22` – SSH
- Ensuring that the attached disk storage (such as persistent EBS volumes on AWS) is 100 GB minimum.
- Having YugabyteDB Anywhere airgapped install package. Contact Yugabyte Support for more information.
- Signing the Yugabyte license agreement. Contact Yugabyte Support for more information.
