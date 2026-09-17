"""Small, dependency-light perception network standing in for the paper's
CUDA PilotNet model. No torch on this Jetson (would require the NVIDIA
Jetson-specific wheel and a large download), so this is a hand-rolled 2-layer
MLP with manual backprop -- real trained weights, real gradients, so FGSM
(threat1_fgsm.py) is a genuine gradient-sign attack, not a fake.

Swap-in path to the paper's real PilotNet/CUDA model: keep the same
`PerceptionModel.predict(frame) -> steer_deg` /
`PerceptionModel.input_gradient(frame, target) -> dL/dframe` interface;
everything else (main_ecu.py, threat scripts) only depends on that interface.
"""
from __future__ import annotations

import numpy as np

FRAME_SIZE = 64          # synthetic camera frame is FRAME_SIZE x FRAME_SIZE, values in [0,1]
INPUT_DIM = FRAME_SIZE * FRAME_SIZE
HIDDEN_DIM = 32
STEER_RANGE_DEG = 35.0   # clean-driving steering stays within a normal lane-keep range


def synthetic_frame(curvature: float, offset: float, noise: float = 0.03,
                     rng: np.random.Generator | None = None) -> np.ndarray:
    """Renders a synthetic road-lane image. `curvature` and `offset` are the
    ground-truth generative factors used only to build the picture and the
    training target -- the network never sees them directly, only pixels."""
    rng = rng or np.random.default_rng()
    xs = np.linspace(-1, 1, FRAME_SIZE)
    ys = np.linspace(0, 1, FRAME_SIZE)
    img = np.zeros((FRAME_SIZE, FRAME_SIZE), dtype=np.float32)
    for row, y in enumerate(ys):
        lane_center = offset + curvature * (y ** 2)
        col = int((lane_center + 1) / 2 * (FRAME_SIZE - 1))
        col = min(max(col, 0), FRAME_SIZE - 1)
        lo, hi = max(0, col - 2), min(FRAME_SIZE, col + 3)
        img[row, lo:hi] = 1.0
    img += rng.normal(0, noise, img.shape).astype(np.float32)
    return np.clip(img, 0.0, 1.0)


def _target_steer(curvature: float, offset: float) -> float:
    raw = curvature * 22.0 + offset * 28.0
    return float(np.clip(raw, -STEER_RANGE_DEG, STEER_RANGE_DEG))


class PerceptionModel:
    def __init__(self, seed: int = 0):
        rng = np.random.default_rng(seed)
        # He-ish init scaled down further: with ~4096 mostly-zero input pixels
        # and LeakyReLU, this keeps pre-activations in a sane range so the
        # network doesn't saturate/die and gradients stay usable for FGSM.
        self.W1 = rng.normal(0, 0.01, (INPUT_DIM, HIDDEN_DIM)).astype(np.float32)
        self.b1 = np.zeros(HIDDEN_DIM, dtype=np.float32)
        self.W2 = rng.normal(0, 0.1, (HIDDEN_DIM, 1)).astype(np.float32)
        self.b2 = np.zeros(1, dtype=np.float32)
        self._trained = False

    def train(self, rng_seed: int = 1, n_samples: int = 2000, epochs: int = 60,
              lr: float = 0.01, batch: int = 32) -> float:
        """Trains on synthetic (frame, steer) pairs. Returns final MAE."""
        rng = np.random.default_rng(rng_seed)
        curv = rng.uniform(-1.0, 1.0, n_samples)
        off = rng.uniform(-1.0, 1.0, n_samples)
        X = np.stack([synthetic_frame(c, o, rng=rng).reshape(-1) for c, o in zip(curv, off)])
        y = np.array([_target_steer(c, o) for c, o in zip(curv, off)], dtype=np.float32)

        n = len(X)
        for _ in range(epochs):
            perm = rng.permutation(n)
            for i in range(0, n, batch):
                idx = perm[i:i + batch]
                xb, yb = X[idx], y[idx]
                pred, cache = self._forward(xb)
                grad_out = 2.0 * (pred - yb) / len(xb)          # dL/dpred
                self._backward(xb, cache, grad_out, lr)

        pred_all, _ = self._forward(X)
        mae = float(np.mean(np.abs(pred_all - y)))
        self._trained = True
        return mae

    def _forward(self, X: np.ndarray):
        z1 = X @ self.W1 + self.b1
        h = np.where(z1 > 0, z1, 0.01 * z1)  # LeakyReLU: never fully saturates/dies,
        pred = (h @ self.W2 + self.b2).reshape(-1)  # so input gradients stay usable for FGSM
        return pred, {"X": X, "z1": z1, "h": h}

    def _backward(self, X, cache, grad_out, lr):
        h = cache["h"]
        z1 = cache["z1"]
        dW2 = h.T @ grad_out.reshape(-1, 1)
        db2 = grad_out.sum(keepdims=True)
        dh = grad_out.reshape(-1, 1) @ self.W2.T
        dz1 = dh * np.where(z1 > 0, 1.0, 0.01)
        dW1 = X.T @ dz1
        db1 = dz1.sum(axis=0)

        self.W2 -= lr * dW2
        self.b2 -= lr * db2
        self.W1 -= lr * dW1
        self.b1 -= lr * db1

    def predict(self, frame: np.ndarray) -> float:
        x = frame.reshape(1, -1).astype(np.float32)
        pred, _ = self._forward(x)
        return float(pred[0])

    def input_gradient(self, frame: np.ndarray, target: float) -> np.ndarray:
        """dL/dframe for L = (predict(frame) - target)^2, used by FGSM.
        NOTE: passing target == predict(frame) gives an all-zero gradient by
        construction (you're already at that point's minimum) -- that is not
        a "no attack possible" result, it just means the caller asked for
        the wrong thing. For an untargeted attack use `output_gradient`
        below and push in the direction that grows |prediction|; for a
        targeted attack pass a genuinely different `target` (e.g. a
        dangerous steering value)."""
        x = frame.reshape(1, -1).astype(np.float32)
        pred, cache = self._forward(x)
        grad_out = 2.0 * (pred - target)  # shape (1,)
        z1 = cache["z1"]
        dh = grad_out.reshape(-1, 1) @ self.W2.T
        dz1 = dh * np.where(z1 > 0, 1.0, 0.01)
        dx = dz1 @ self.W1.T
        return dx.reshape(frame.shape)

    def output_gradient(self, frame: np.ndarray) -> np.ndarray:
        """dPred/dframe (raw output Jacobian, not a loss to a target).
        Used for untargeted FGSM: stepping along sign(output_gradient) *
        sign(pred) grows |pred| directly, i.e. makes whatever the model is
        already leaning towards more extreme -- a standard untargeted attack
        shape for a scalar regression output where no separate ground-truth
        label is available to the attacker."""
        x = frame.reshape(1, -1).astype(np.float32)
        pred, cache = self._forward(x)
        z1 = cache["z1"]
        dh = np.ones((1, 1), dtype=np.float32) @ self.W2.T
        dz1 = dh * np.where(z1 > 0, 1.0, 0.01)
        dx = dz1 @ self.W1.T
        return dx.reshape(frame.shape), float(pred[0])


_MODEL: PerceptionModel | None = None


def get_trained_model() -> PerceptionModel:
    global _MODEL
    if _MODEL is None:
        m = PerceptionModel(seed=0)
        mae = m.train()
        print(f"[perception] trained tiny perception net, final MAE={mae:.3f} deg")
        _MODEL = m
    return _MODEL
