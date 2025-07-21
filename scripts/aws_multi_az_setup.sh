#!/bin/bash
# Provision multi-AZ infrastructure with EC2 instances supporting KVM

set -e

usage() {
    echo "Usage: $0 --regions region1a,region2b" >&2
    exit 1
}

REGIONS_CSV=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --regions)
            REGIONS_CSV="$2"
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

if [[ -z "$REGIONS_CSV" ]]; then
    usage
fi

IFS=',' read -ra AZS <<< "$REGIONS_CSV"

VPCS=()
SUBNETS=()
INSTANCES=()

CIDR_INDEX=1
for AZ in "${AZS[@]}"; do
    REGION=${AZ::-1}
    echo "Creating VPC and subnet in $AZ"
    VPC_ID=$(aws ec2 create-vpc --cidr-block 10.${CIDR_INDEX}.0.0/16 --query 'Vpc.VpcId' --output text --region "$REGION")
    aws ec2 modify-vpc-attribute --vpc-id "$VPC_ID" --enable-dns-support '{"Value":true}' --region "$REGION"
    SUBNET_ID=$(aws ec2 create-subnet --vpc-id "$VPC_ID" --cidr-block 10.${CIDR_INDEX}.0.0/24 --availability-zone "$AZ" --query 'Subnet.SubnetId' --output text --region "$REGION")
    SG_ID=$(aws ec2 create-security-group --group-name virtio-nic-sg-"$AZ" --description "Virtio NIC SG" --vpc-id "$VPC_ID" --output text --region "$REGION")
    aws ec2 authorize-security-group-ingress --group-id "$SG_ID" --protocol tcp --port 22 --cidr 0.0.0.0/0 --region "$REGION"
    echo "Launching instance in $AZ"
    INSTANCE_ID=$(aws ec2 run-instances --image-id ami-12345678 --instance-type c5.large --key-name mykey --security-group-ids "$SG_ID" --subnet-id "$SUBNET_ID" --query 'Instances[0].InstanceId' --output text --region "$REGION")
    VPCS+=("$VPC_ID")
    SUBNETS+=("$SUBNET_ID")
    INSTANCES+=("$INSTANCE_ID")
    CIDR_INDEX=$((CIDR_INDEX+1))
    echo "Instance $INSTANCE_ID running in $AZ"

    # Tag for easy identification
    aws ec2 create-tags --resources "$INSTANCE_ID" --tags Key=Name,Value=virtio-nic-test --region "$REGION"
done

# Set up VPC peering between all VPCs
for ((i=0;i<${#VPCS[@]}-1;i++)); do
    for ((j=i+1;j<${#VPCS[@]};j++)); do
        REGION1=${AZS[$i]::-1}
        REGION2=${AZS[$j]::-1}
        echo "Creating VPC peering between ${VPCS[$i]} and ${VPCS[$j]}"
        PEER_ID=$(aws ec2 create-vpc-peering-connection --vpc-id "${VPCS[$i]}" --peer-vpc-id "${VPCS[$j]}" --region "$REGION1" --peer-region "$REGION2" --query 'VpcPeeringConnection.VpcPeeringConnectionId' --output text)
        aws ec2 accept-vpc-peering-connection --vpc-peering-connection-id "$PEER_ID" --region "$REGION2"

        RT1=$(aws ec2 describe-route-tables --filters Name=vpc-id,Values="${VPCS[$i]}" --region "$REGION1" --query 'RouteTables[0].RouteTableId' --output text)
        RT2=$(aws ec2 describe-route-tables --filters Name=vpc-id,Values="${VPCS[$j]}" --region "$REGION2" --query 'RouteTables[0].RouteTableId' --output text)

        DEST1="10.$((j+1)).0.0/16"
        DEST2="10.$((i+1)).0.0/16"
        aws ec2 create-route --route-table-id "$RT1" --destination-cidr-block "$DEST1" --vpc-peering-connection-id "$PEER_ID" --region "$REGION1"
        aws ec2 create-route --route-table-id "$RT2" --destination-cidr-block "$DEST2" --vpc-peering-connection-id "$PEER_ID" --region "$REGION2"
    done
done

echo "Infrastructure setup complete. Instance IDs: ${INSTANCES[*]}"
