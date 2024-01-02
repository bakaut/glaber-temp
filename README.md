# Please go to [Glaber docs](https://docs.glaber.io)

# glaber quick start
```bash
 # needs root access
sudo su
cd /opt/
git clone --depth 1 --branch master https://gitlab.com/mikler/glaber.git
# docker folder is an appliance type, 
# possible variants virtualbox,yandex-cloud-vm,vagrant,helm-chart etc
cd glaber/build/appliance/run/docker
# result of command is ready to use 
# glaber application with all needed components
# deployed in docker-compose
./glaber.sh start
```

Available options to install glaber:
- `./glaber.sh start stable`,   - install latest stable version
- `./glaber.sh start latest`,   - install latest version
- `./glaber.sh start 3.0.50`,   - install a certain glaber version
- `./glaber.sh upgrade 3.0.50`, - upgrade to the certain glaber version
- `./glaber.sh upgrade`       , - upgrade to the latest glaber version

By default, glaber install a latest stable version

# Install glaber on baremetal host or virtual mashine 

Сompatibility matrix (for installation with ansible):
| OS |  Support |   
|---|---|
| Debian 10 (Buster) |  F |
| Debian 11 (Bullseye) | F | 
| Debian 12 (Bookworm) | F  |   
| Ubuntu 18.04 LTS (Bionic Beaver | P |  
| Ubuntu 20.04 LTS (Focal Fossa) |  F |  
| Ubuntu 22.04 LTS (Jammy Jellyfish)| F |   

- F - Fully supported (tested installation with mysql and postgres variant)
- P - Partial supported (some restrictions)

Limitation:
- Ubuntu 18.04 Needs to update libc > 2.29 and php > 7.4 version

Recommended software requirements:

OS:
- Debian bullseye


Recommended starting hardware requirements:

Resources:
- 1 CPU
- 8G RAM
- 50G SSD

1. Configure passwordless access(ssh key) to you remote bare metal server (or VM):
```bash
# on your desctop
ssh-keygen -t ed25519 -f ~/.ssh/glaber
# get current user name
id -nu
# connect to remote server with root user and create user name the same as `id -nu`
ssh root@<remote_server>
useradd <username> -g root
# configure grant privileges without the password
# visudo
<username> ALL=(ALL) NOPASSWD: /usr/bin/sudo
# return to your desctop
ssh-copy-id -i ~/.ssh/glaber.pub <username>@<remote_server>
```
2. Download this repo:
```bash
git clone --depth 1 --branch master https://gitlab.com/mikler/glaber.git
```
3. Change ip address in `glaber/build/appliance/run/ansible/inventory/hosts.ini` to your <remote_server>
4. Adjust the variables.
   Please change the passwords and glaber_build_version before installing.
   Do not leave it as is.
```bash
build/appliance/run/ansible/inventory/group_vars/all/vars.yml
```
5. Run ansible-playbook to install glaber all in one server:
```bash
apt-get install -y ansible
cd glaber/build/appliance/run/ansible
# By default installation of glaber mysql version
ansible-playbook glaber.yaml

# If you need postresql variant, type
ansible-playbook -e "glaber_db_type=postgres" glaber.yaml
```

# Install glaber with pre-congigured VM (kvm)
1. Download glaber vm disk (kvm, qcow2 extention )
https://glaber-vms.website.yandexcloud.net/
2. Install kvm hypervisor
https://ubuntu.com/blog/kvm-hyphervisor
3. Install virt-manager
https://ubuntu.com/server/docs/virtualization-virt-tools
4. Create the vm using the  virt-manager
