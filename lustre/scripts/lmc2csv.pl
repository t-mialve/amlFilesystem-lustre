#!/usr/bin/perl

# vim:expandtab:shiftwidth=4:softtabstop=4:tabstop=4:

#
# convert an lmc batch file to a csv file for lustre_config.sh
#
use strict; use warnings;

use Data::Dumper;

sub get_arg_val {
    my $arg = shift;
    my ($aref) = @_;
    for (my $i = 0; $i <= $#$aref; $i++) {
        if ($$aref[$i] eq "--" . $arg) {
            my @foo = splice(@$aref, $i, 2);
            return $foo[1];
        }
    }
}

sub get_arg {
    my $arg = shift;
    my ($aref) = @_;
    for (my $i = 0; $i <= $#$aref; $i++) {
        if ($$aref[$i] eq "--" . $arg) {
            splice(@$aref, $i, 1);
            return 1;
        }
    }

    return 0;
}

sub add_net {
    my $net = {};
    $net->{"node"} = get_arg_val("node", \@_);
    $net->{"nid"} = get_arg_val("nid", \@_);
    $net->{"nettype"} = get_arg_val("nettype", \@_);
    $net->{"port"} = get_arg_val("port", \@_);
    if ($#_ > 0) {
        print STDERR "Unknown arguments to \"--add net\": @_\n";
        exit(1);
    }
    return $net;
}

sub add_mds {
    my $mds = {};
    $mds->{"node"} = get_arg_val("node", \@_);
    $mds->{"name"} = get_arg_val("mds", \@_);
    $mds->{"fstype"} = get_arg_val("fstype", \@_);
    $mds->{"dev"} = get_arg_val("dev", \@_);
    $mds->{"size"} = get_arg_val("size", \@_);
    $mds->{"lmv"} = get_arg_val("lmv", \@_);
    if ($#_ > 0) {
        print STDERR "Unknown arguments to \"--add mds\": @_\n";
        exit(1);
    }
    return $mds;
}

sub add_lov {
    my $lov = {};
    $lov->{"name"} = get_arg_val("lov", \@_);
    $lov->{"mds"} = get_arg_val("mds", \@_);
    $lov->{"lmv"} = get_arg_val("lmv", \@_);
    $lov->{"stripe_sz"} = get_arg_val("stripe_sz", \@_);
    $lov->{"stripe_cnt"} = get_arg_val("stripe_cnt", \@_);
    $lov->{"stripe_pattern"} = get_arg_val("stripe_pattern", \@_);
    if ($#_ > 0) {
        print STDERR "Unknown arguments to \"--add lov\": @_\n";
        exit(1);
    }
    return $lov;
}

sub add_ost {
    my $ost = {};
    $ost->{"node"} = get_arg_val("node", \@_);
    $ost->{"name"} = get_arg_val("ost", \@_);
    $ost->{"fstype"} = get_arg_val("fstype", \@_);
    $ost->{"dev"} = get_arg_val("dev", \@_);
    $ost->{"size"} = get_arg_val("size", \@_);
    $ost->{"lov"} = get_arg_val("lov", \@_);
    $ost->{"mountfsoptions"} = get_arg_val("mountfsoptions", \@_);
    $ost->{"failover"} = get_arg("failover", \@_);
    $ost->{"failout"} = get_arg("failout", \@_);
    if ($#_ > 0) {
        print STDERR "Unknown arguments to \"--add ost\": @_\n";
        exit(1);
    }
    return $ost;
}

sub add_mtpt {
    my $mtpt = {};
    $mtpt->{"node"} = get_arg_val("node", \@_);
    $mtpt->{"path"} = get_arg_val("path", \@_);
    $mtpt->{"mds"} = get_arg_val("mds", \@_);
    $mtpt->{"lov"} = get_arg_val("lov", \@_);
    $mtpt->{"lmv"} = get_arg_val("lmv", \@_);
    if ($#_ > 0) {
        print STDERR "Unknown arguments to \"--add mtpt\": @_\n";
        exit(1);
    }
    return $mtpt;
}

no strict 'refs';

sub find_objs {
    my $type = shift;
    my $key = shift;
    my $value = shift;
    my @objs = @_;

    my @found_objs;
    foreach my $obj (@objs) {
        if ($obj->{$key} eq $value) {
            push(@found_objs, $obj);
        }
    }

    return @found_objs;
}

sub lnet_options {
    my $net = shift;

    my $options_str = "options lnet networks=" . $net->{"nettype"} .
                   " accept=all";
    if (defined($net->{"port"})) {
        $options_str .= " accept_port=" . $net->{"port"};
    }
    return $options_str;

}

# main

my %objs;
my @mgses;

my $MOUNTPT = "/mnt";
if (defined($ENV{"MOUNTPT"})) {
    $MOUNTPT = $ENV{"MOUNTPT"};
}

while(<>) {
    my @args = split;

    for (my $i = 0; $i <= $#args; $i++) {
        if ($args[$i] eq "--add") {
            my $type = "$args[$i + 1]";
            my $subref = "add_$type";
            splice(@args, $i, 2);
            push(@{$objs{$type}}, &$subref(@args));
            last;
        }
        if ($i == $#args) {
            print STDERR "I don't know how to handle @args\n";
            exit(1);
        }
    }
}

# link lovs to mdses
foreach my $lov (@{$objs{"lov"}}) {
    foreach my $mds (find_objs("mds", "name", $lov->{"mds"}, @{$objs{"mds"}})) {
        if ($mds) {
            $mds->{"lov"} = $lov;
        }
    }
    # try via lmvs as well
    foreach my $mds (find_objs("mds", "lmv", $lov->{"lmv"}, @{$objs{"mds"}})) {
        if ($mds) {
            $mds->{"lov"} = $lov;
        }
    }
}

# create lmvs and link them to mdses
foreach my $mds (@{$objs{"mds"}}) {
    my $lmv;
    my @lmvs = find_objs("lmv", "name", $mds->{"lmv"}, @{$objs{"lmv"}});
    if ($#lmvs < 0) {
        $lmv = {};
        $lmv->{"name"} = $mds->{"lmv"};
        push(@{$objs{"lmv"}}, $lmv);
    } else {
        $lmv = pop(@lmvs);
    }
    $mds->{"lmv"} = $lmv;
}

# link mtpts to lovs and lmvs or mdses
foreach my $mtpt (@{$objs{"mtpt"}}) {
    foreach my $mds (find_objs("mds", "name", $mtpt->{"mds"}, @{$objs{"mds"}})) {
        if ($mds) {
            $mds->{"mtpt"} = $mtpt;
        }
    }
    foreach my $lmv (find_objs("lmv", "name", $mtpt->{"lmv"}, @{$objs{"lmv"}})) {
        if ($lmv) {
            $lmv->{"mtpt"} = $mtpt;
        }
    }
    foreach my $lov (find_objs("lov", "name", $mtpt->{"lov"}, @{$objs{"lov"}})) {
        if ($lov) {
            $lov->{"mtpt"} = $mtpt;
        }
    }
}

# XXX could find failover pairs of osts and mdts here and link them to
# one another and then fill in their details in the csv generators below
my $COUNT = 1;
foreach my $mds (@{$objs{"mds"}}) {
    # find the net for this node
    my @nets = find_objs("net", "node", $mds->{"node"}, @{$objs{"net"}});
    my $lmv = $mds->{"lmv"};
    my $lov = $mds->{"lov"};
    my $mtpt;
    if ($lmv) {
        $mtpt = $mds->{"lmv"}->{"mtpt"};
    } else {
        $mtpt = $mds->{"mtpt"};
    }
    my $mkfs_options="";
    if (defined($lov->{"stripe_sz"})) {
        $mkfs_options .= "lov.stripesize=" . $lov->{"stripe_sz"} . " ";
    }
    if (defined($lov->{"stripe_cnt"})) {
        $mkfs_options .= "lov.stripecount=" . $lov->{"stripe_cnt"} . " ";
    }
    if (defined($lov->{"stripe_pattern"})) {
        $mkfs_options .= "lov.stripetype=" . $lov->{"stripe_pattern"} . " ";
    }
    chop($mkfs_options);
    if ($mkfs_options ne "") {
        $mkfs_options = " --param=\"$mkfs_options\"";
    }

    my $mount_point = "$MOUNTPT/" . $mds->{"name"};
    if (defined($mtpt->{"node"})) {
        $mkfs_options .= " --fsname=\"" . $mtpt->{"node"} . "\" ";
        $mount_point .= "_" . $mtpt->{"node"};
    }
    $mkfs_options =~ s/\s*$//;

    if ($COUNT == 1) {
        # mgs/mdt
        printf "%s,%s,%s,%s,mgs|mdt,,,,--device-size=%s --noformat%s,,noauto\n", 
        $mds->{"node"},
        lnet_options($nets[0]),
        $mds->{"dev"},
        $mount_point,
        $mds->{"size"},
        $mkfs_options;

        push(@mgses, $nets[0]->{"nid"});
    } else {
        # mdt
        printf "%s,%s,%s,%s,mdt,,\"%s\",,--device-size=%s --noformat%s,,noauto\n",
        $mds->{"node"},
        lnet_options($nets[0]),
        $mds->{"dev"},
        $mount_point,
        join(",", @mgses),
        $mds->{"size"},
        $mkfs_options;
    }
    $COUNT++;
}

foreach my $ost (@{$objs{"ost"}}) {
    my $mount_opts="noauto";
    if (defined($ost->{"mountfsoptions"})) {
        $mount_opts .= "," . $ost->{"mountfsoptions"};
    }
    my $mkfs_options="";
    if (defined($ost->{"failover"}) & $ost->{"failover"}) {
        $mkfs_options .= "failover.mode=failover" . " ";
    }
    if (defined($ost->{"failout"}) & $ost->{"failout"}) {
        $mkfs_options .= "failover.mode=failout" . " ";
    }
    chop($mkfs_options);
    if ($mkfs_options ne "") {
        $mkfs_options = " --param=\"$mkfs_options\"";
    }
    
    $ost->{"lov"} = (find_objs("lov", "name", $ost->{"lov"}, @{$objs{"lov"}}))[0];
    my $mount_point = "$MOUNTPT/" . $ost->{"name"}, 
    my $mtpt = $ost->{"lov"}->{"mtpt"};
    if (defined($mtpt->{"node"})) {
        $mkfs_options .= " --fsname=\"" . $mtpt->{"node"} . "\" ";
        $mount_point .= "_" . $mtpt->{"node"};
    }
    $mkfs_options =~ s/\s*$//;
    # find the net for this node
    my @nets = find_objs("net", "node", $ost->{"node"}, @{$objs{"net"}});
    printf "%s,%s,%s,%s,ost,,\"%s\",,--device-size=%s --noformat%s,,\"%s\"\n", 
    $ost->{"node"},
    lnet_options($nets[0]),
    $ost->{"dev"},
    $mount_point,
    join(",", @mgses),
    $ost->{"size"},
    $mkfs_options,
    $mount_opts;
}
