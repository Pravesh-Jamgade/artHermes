#!/usr/bin/env python3
"""
kmeans_ppki.py

Reads your window-metrics CSV and runs k-means on:
  - ppki4k
  - new_frac
  - churn = 1 - jac

Writes output CSV with 2 extra columns:
  - cluster_id
  - hot (1 if in hottest cluster else 0)

Usage:
  python kmeans_ppki.py --in input/ --out clustered.csv --k 4

Notes:
  - "hot" cluster is chosen as argmax of stress_score = mean(ppki4k) * mean(churn)
"""

import argparse
import pandas as pd
from sklearn.cluster import KMeans
from sklearn.preprocessing import StandardScaler
from pathlib import Path
import os.path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True, help="Input CSV path")
    # ap.add_argument("--out", dest="out", required=True, help="Output CSV path")
    ap.add_argument("--k", type=int, default=4, help="Number of clusters (default: 4)")
    ap.add_argument("--seed", type=int, default=0, help="Random seed (default: 0)")
    ap.add_argument("--n_init", type=int, default=30, help="KMeans n_init (default: 30)")
    args = ap.parse_args()
    
    files_in_folder = []
    path_string = args.inp
    if os.path.isfile(path_string):
        print(f"'{path_string}' is a file.")
        files_in_folder.append(path_string)
    elif os.path.isdir(path_string):
        print(f"'{path_string}' is a directory.")
        folder_path = Path(args.inp)
        # Get a list of all files (excluding directories)
        files_in_folder = [p for p in folder_path.iterdir() if p.is_file()]
    

    for f in files_in_folder:
        print(f"Processin {f}")
        run(f, args)

def run(inputFile, args):
    name = Path(inputFile).name
    df = pd.read_csv(inputFile)

    # Basic validation
    required = ["ppki4k", "new_frac", "jac"]
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise SystemExit(f"Missing required columns: {missing}")

    # Feature engineering
    df = df.copy()
    df["churn"] = 1.0 - df["jac"]

    # Build feature matrix
    feat_cols = ["ppki4k", "new_frac", "churn"]
    X = df[feat_cols].astype(float).values

    # Normalize (important for k-means)
    scaler = StandardScaler()
    Xs = scaler.fit_transform(X)

    # KMeans
    km = KMeans(n_clusters=args.k, n_init=args.n_init, random_state=args.seed)
    df["cluster_id"] = km.fit_predict(Xs).astype(int)

    # Choose hot cluster using cluster-level stress_score = mean(ppki4k) * mean(churn)
    centers = (
        df.groupby("cluster_id")[["ppki4k", "new_frac", "churn"]]
        .mean()
        .rename(columns={"ppki4k": "mean_ppki4k", "new_frac": "mean_new_frac", "churn": "mean_churn"})
    )
    centers["stress_score"] = centers["mean_ppki4k"] * centers["mean_churn"]
    hot_cluster_id = int(centers["stress_score"].idxmax())

    # Mark hot windows
    df["hot"] = (df["cluster_id"] == hot_cluster_id).astype(int)

    # Write output (keeps all original cols, plus churn + cluster_id + hot)
    df.to_csv("output_"+name, index=False)

    # Print a small summary
    print("Wrote:", "output_"+name)
    print("k =", args.k)
    print("hot_cluster_id =", hot_cluster_id)
    print("\nCluster summary (means):")
    print(centers.sort_values("stress_score", ascending=False).round(6).to_string())


if __name__ == "__main__":
    main()
