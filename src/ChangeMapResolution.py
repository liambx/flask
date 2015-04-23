#! /usr/bin/env python

# This script gets a Healpix map in FITS file <IN>, changes its 
# resolution (Nside) according to <NSIDE> and write the new map 
# to <OUT>.
# USAGE: ./ChangeMapResolution.py <IN> <NSIDE> <OUT>

import healpy as hp
import sys

# Get input:
mapIn    =     sys.argv[1]
newNside = int(sys.argv[2])
mapOut   =     sys.argv[3]

# Operate on map:
mapx = hp.read_map(mapIn)
mapy = hp.ud_grade(mapx, nside_out=newNside)
print "NEW NSIDE =", newNside

# Write it out:
hp.write_map(mapOut, mapy)
print "New map written to", mapOut


